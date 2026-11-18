/*
 * slopsix_eg.c - The amplitude envelope and the level pipeline.
 *
 * The pipeline is the hardware's own: everything is composed in the Q8
 * log2 domain (256 units per octave) and converted once, at the signal
 * exponent.  The chain is
 *
 *   log_amp = envelope level (Q8)
 *            + carrier-count compensation (Q8, negative)
 *            + amplitude modulation (Phase 4)
 *
 * and the envelope's own target is composed of
 *
 *   target = the stage's level curve (Q8)
 *          + the operator's level: the output level, the keyboard level
 *            scaling and the velocity (Q8)
 *
 * The units of the level parameters are the hardware's: the operator
 * output level and the envelope's stage levels are 0..99 parameters, the
 * keyboard level scaling's depths are 0..99 and the velocity sensitivity
 * is 0..7.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
 */
#include <string.h>

#include "slopsix_internal.h"

/* --------------------------------------------------- the hardware's tables */

/* The keyboard level scaling's linear curve: 36 entries, the magnitude
 * of the level change at one, two, ... groups away from the breakpoint.
 * The table saturates at its top (research E-05). */
static const uint8_t slopsix_kls_curve_lin[36] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 72,
    80, 88, 96, 104, 112, 120, 128, 136, 144, 152,
    160, 168, 178, 184, 192, 200, 208, 216, 224, 232,
    240, 248, 255, 255, 255, 255
};

/* The keyboard level scaling's exponential curve: 36 entries, the
 * magnitude of the level change at one, two, ... groups away from the
 * breakpoint (research E-05). */
static const uint8_t slopsix_kls_curve_exp[36] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    11, 14, 16, 19, 23, 28, 33, 39, 47, 57,
    67, 80, 95, 113, 134, 160, 190, 224, 255, 255,
    255, 255, 255, 255, 255, 255
};

/* The velocity route's attenuation table: 32 entries, the attenuation
 * the operator volume carries for one velocity group.  The table rises
 * with the index, and the engine indexes it from the hard end (see
 * slopsix_velocity_attenuation).
 *
 * The first entry is the hardware's own word (the value 4 in the low
 * byte and a zero in the high byte); the dip at the second entry is
 * reproduced as the hardware has it (research E-07).
 *
 * The thirty-third entry is the byte that follows the table in the
 * firmware's ROM: the firmware indexes the table at 32 - (velocity >> 2),
 * so the softest key (group 0) reads one entry past the table's end.
 * That byte is the opcode of the routine that follows the table, and the
 * firmware's velocity response for the softest keys depends on it. */
static const uint8_t slopsix_velocity_attenuation_table[33] = {
    4, 0, 12, 21, 30, 40, 46, 52,
    58, 64, 70, 76, 82, 88, 94, 100,
    103, 106, 109, 112, 114, 116, 118, 120,
    122, 124, 126, 128, 130, 131, 132, 133,
    189
};

/* The measured stage durations: 77 entries, the number of samples a
 * stage whose target is the current level lasts, measured at 44.1 kHz
 * (research E-02).  The values are re-anchored to the engine's own rate
 * when they are used, and the rates beyond the measured range are
 * approximated (research E-02).
 *
 * Evidence grade: measured model (hypothesis-grade, plan section 3.0). */
static const int32_t slopsix_eg_statics[77] = {
    1764000, 1764000, 1411200, 1411200, 1190700, 1014300, 992250,
    882000, 705600, 705600, 584325, 507150, 502740, 441000, 418950,
    352800, 308700, 286650, 253575, 220500, 220500, 176400, 145530,
    145530, 125685, 110250, 110250, 88200, 88200, 74970, 61740,
    61740, 55125, 48510, 44100, 37485, 31311, 30870, 27562, 27562,
    22050, 18522, 17640, 15435, 14112, 13230, 11025, 9261, 9261, 7717,
    6615, 6615, 5512, 5512, 4410, 3969, 3969, 3439, 2866, 2690, 2249,
    1984, 1896, 1808, 1411, 1367, 1234, 1146, 926, 837, 837, 705,
    573, 573, 529, 441, 441
};

/* The rate at which the measured stage durations were taken. */
#define SLOPSIX_EG_MODEL_RATE 44100u

/* ---------------------------------------------------------- the level curve */

/*
 * The level curve.
 *
 * The hardware's level parameters (the operator output level and the
 * envelope's stage levels) run from 0 to 99 and map onto a gain of 0 to
 * 127 microsteps, where one microstep is 1/8 of an octave.  The law is
 * closed: below 20 the gain follows the hardware's own curve, from 20
 * upwards it is linear.
 *
 * The hardware keeps the curve in the other direction: its own table is
 * an attenuation table, and it is the exact inverse of the law below
 * (research E-08).  The engine works in the gain direction, because
 * every other contribution in the pipeline is a gain.
 */
int32_t slopsix_level_curve(uint8_t param)
{
    static const uint8_t low[20] = {
        0, 5, 9, 13, 17, 20, 23, 25, 27, 29,
        31, 33, 35, 37, 39, 41, 42, 43, 45, 46
    };

    if (param >= 20u) {
        return 28 + (int32_t)param;
    }
    return low[param];
}

/* ------------------------------------------------------- the note's group */

/*
 * The note's key-code group.
 *
 * The keyboard level scaling's curve is indexed by the key code, in
 * groups of three semitones: the whole keyboard spans 43 groups.  The
 * group is the key code's quarter, which the engine's note word carries
 * directly (the word is the key code in its high byte).
 */
uint8_t slopsix_note_group(uint8_t note)
{
    return (uint8_t)((slopsix_note_word[note & 127u] >> 10) & 0xFFu);
}

/* -------------------------------------------- the keyboard level scaling */

/*
 * The breakpoint's group.
 *
 * The hardware's breakpoint parameter counts from the lowest breakpoint,
 * which it offsets by twenty keys before it reads the key-code table;
 * the group is the key code's quarter, like the note's.  The curve's
 * index runs from one to 43, so the breakpoint's index is the group's
 * plus one: the note that lands on the breakpoint is the one whose
 * index equals it, and it is the neutral point of the curve.
 */
uint8_t slopsix_kls_breakpoint_index(uint8_t breakpoint)
{
    return (uint8_t)((slopsix_note_t[(uint8_t)(breakpoint + 20u) & 127u] >> 2)
                      + 1u);
}

/*
 * The keyboard level scaling builder.
 *
 * The hardware builds a per-operator curve of 43 entries at patch
 * activation time, one entry per key-code group, and reads the entry
 * that the played note's group selects at note-on.  Each entry is the
 * operator's level, corrected by the depth of the side of the
 * breakpoint the note is on.
 *
 * The correction's magnitude is the curve table's value at the distance
 * from the breakpoint, scaled by the side's depth; the sign follows the
 * side's curve: a negative curve lowers the level away from the
 * breakpoint, a positive one raises it.
 *
 * The engine builds the *offset* rather than the level, so that the
 * operator's output level can be applied on top of it.
 */
void slopsix_kls_build(int8_t *offsets, uint8_t breakpoint,
    uint8_t left_depth, uint8_t right_depth,
    uint8_t left_curve, uint8_t right_curve)
{
    int group;
    int bp;

    bp = slopsix_kls_breakpoint_index(breakpoint);

    for (group = 0; group < SLOPSIX_KLS_GROUPS; group++) {
        const uint8_t *table;
        uint8_t curve;
        uint8_t depth;
        int distance;
        int magnitude;

        /* The hardware's curve index runs from 1 to 43; the breakpoint's
         * own group is the zero of the distance. */
        if (group + 1 > bp) {
            /* Above the breakpoint: the right curve. */
            distance = group + 1 - bp;
            curve = right_curve;
            depth = right_depth;
        } else {
            distance = bp - (group + 1);
            curve = left_curve;
            depth = left_depth;
        }

        /* The linear curve is used by the two linear curve types, the
         * exponential one by the two exponential types. */
        table = ((curve == 0u) || (curve == 3u))
                    ? slopsix_kls_curve_lin : slopsix_kls_curve_exp;

        /* The curve tables saturate at their top; the hardware reads
         * past them, the engine saturates at the last entry. */
        if (distance > 35) {
            distance = 35;
        }

        /* The depth is the hardware's scaled value: the parameter times
         * 660, in its high byte. */
        magnitude = ((int32_t)table[distance] *
            (((int32_t)depth * 660) >> 8)) >> 8;
        if (magnitude > SLOPSIX_MICROSTEP_MAX) {
            magnitude = SLOPSIX_MICROSTEP_MAX;
        }

        /* The curve types 0 and 1 are the negative ones, 2 and 3 the
         * positive ones. */
        if (curve >= 2u) {
            offsets[group] = (int8_t)magnitude;
        } else {
            offsets[group] = (int8_t)(-magnitude);
        }
    }
}

/* ------------------------------------------------------- the velocity route */

/*
 * The velocity route.
 *
 * The hardware scales the operator volume by the key velocity: the
 * operator's velocity sensitivity gives a coefficient, the velocity
 * itself selects an entry of the attenuation table, and the two are
 * combined with the sensitivity's offset (research E-07).
 *
 * The attenuation table rises with the velocity, and the operator volume
 * register is an attenuation (the hardware adds the keyboard level
 * scaling's correction to it and mutes the operator when the sum
 * overflows), so the table has to be read from its hard end for the
 * route to be audible: a harder key must produce a *louder* operator.
 * That direction is the one the engine implements (research E-07).
 */
int32_t slopsix_velocity_attenuation(uint8_t velocity, uint8_t sensitivity)
{
    uint8_t sens = sensitivity > 7u ? 7u : sensitivity;
    uint32_t group = (uint32_t)(velocity & 127u) >> 2;
    int32_t scale;
    int32_t attenuation;

    /* The firmware reads the table backwards, at 32 - (velocity >> 2),
     * so the softest key reads the byte that follows the table (see the
     * table's comment). */
    scale = slopsix_velocity_attenuation_table[32u - group];

    /* The sensitivity's offset and its coefficient. */
    attenuation = (15 - 2 * (int32_t)sens) +
                  (((int32_t)sens * 32 * scale) >> 8);

    return attenuation;
}

/* ------------------------------------------------------ the operator level */

/*
 * The operator's level.
 *
 * The output level and the keyboard level scaling's correction compose
 * the operator's gain, in microsteps; the velocity's attenuation is
 * subtracted from it, in the Q8 domain.  The hardware's register holds
 * the keyboard level scaling's part twice, so one microstep of the
 * operator's level is 32 units of the Q8 domain and one unit of the
 * velocity's attenuation is 16.
 */
int32_t slopsix_op_level_q8(uint8_t out_level, int8_t kls_offset,
    uint8_t velocity, uint8_t velocity_sens)
{
    int32_t gain = slopsix_level_curve(out_level) + (int32_t)kls_offset;
    int32_t attenuation;

    if (gain < 0) {
        gain = 0;
    }
    if (gain > SLOPSIX_MICROSTEP_MAX) {
        gain = SLOPSIX_MICROSTEP_MAX;
    }

    attenuation = slopsix_velocity_attenuation(velocity, velocity_sens);

    return (gain << SLOPSIX_MICROSTEP_Q8_SHIFT) -
           (attenuation << SLOPSIX_VELOCITY_Q8_SHIFT);
}

/* ------------------------------------------------------------ the rate scaling */

/*
 * The rate scaling.
 *
 * The keyboard's rate scaling makes an envelope faster the further the
 * note is from the bottom of the keyboard: the contribution grows with
 * the note's group above the lowest octave and saturates (research
 * E-03).
 */
uint8_t slopsix_rate_scaling(uint8_t note, uint8_t sensitivity)
{
    uint8_t sens = sensitivity > 7u ? 7u : sensitivity;
    int32_t x = ((int32_t)(note & 127u) / 3) - 7;

    if (x < 0) {
        x = 0;
    }
    if (x > 31) {
        x = 31;
    }

    /* The EGS's key-scaling law, as the bit-synchronous model carries it:
     * round(x * sens / 7), in qrate units.  A BANJO trace (2026-11-18)
     * gives 6 for sens 3 and 13 for sens 7; the old >>3 form gave 4 and
     * 11 and left the envelope too slow.  The note-dependent x is still
     * the engine's approximation (note / 3 - 7); the EGS derives it from
     * the voice-pitch word. */
    return (uint8_t)(((int32_t)sens * x + 3) / 7);
}

/* ------------------------------------------ the carrier-count compensation */

/* ------------------------------------------------------------ the envelope */

/*
 * The rate's increment.
 *
 * The model turns a rate and the note's rate scaling into an increment
 * of the envelope's level per sample (research E-01):
 *
 *   q   = (rate * 41) >> 6, plus the rate scaling
 *   inc = (4 + (q & 3)) << (8 + (q >> 2))
 *
 * The increment is in the model's own 44.1 kHz domain and is re-anchored
 * to the host's rate.
 */
static int32_t slopsix_eg_scale(int32_t value, uint32_t sample_rate)
{
    int64_t scaled;

    if (sample_rate == 0u) {
        sample_rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    }

    /* A host rate far below the model's own pushes the fastest stage's
     * increment past the word's range; the conversion saturates instead
     * of wrapping, so the behaviour stays deterministic. */
    scaled = ((int64_t)value * (int64_t)SLOPSIX_EG_MODEL_RATE +
        (int64_t)(sample_rate / 2u)) / (int64_t)sample_rate;

    if (scaled > INT32_MAX) {
        return INT32_MAX;
    }
    if (scaled < INT32_MIN) {
        return INT32_MIN;
    }

    return (int32_t)scaled;
}

/*
 * The stage's target and its increment.
 *
 * The target is the stage's level, composed with the operator's own
 * level, in the Q8 domain; the envelope's level is held in that domain
 * with 16 fractional bits.  A target below the domain's floor is
 * clamped there.
 */
static void slopsix_eg_advance(slopsix_eg *eg, uint8_t stage)
{
    int32_t target;
    int32_t q;
    int32_t inc;
    uint8_t rate;
    uint8_t level_param;

    eg->stage = stage;
    if (stage > 3u) {
        /* The envelope has run out of stages. */
        eg->static_count = 0;
        return;
    }

    rate = eg->rates[stage];
    level_param = eg->levels[stage];

    /* The target. */
    target = ((slopsix_level_curve(level_param) >> 1) << 6) +
             eg->op_level_q8 - SLOPSIX_LEVEL_OFFSET_Q8;
    if (target < SLOPSIX_EG_MIN_Q8) {
        target = SLOPSIX_EG_MIN_Q8;
    }
    eg->target = target << SLOPSIX_EG_FRAC;
    eg->rising = (eg->target > eg->level) ? 1u : 0u;

    /* The increment.
     *
     * The rate gate.  The firmware pre-quantises the patch's 0..99 rate
     * with (rate * 164) >> 8 (chip register domain 0..63); the engine
     * adds the note's rate scaling and forms
     *
     *   q = (rate * 41) >> 6 + rate scaling
     *
     * which is the firmware expression rewritten in the engine's Q6
     * domain.  The increment doubles every four q steps and the low two
     * bits select the fraction 4/4..7/4 (research E-02).  A
     * bit-synchronous EGS model follows the same law, and the chip's
     * documented envelope time table agrees with it across the rate
     * range.  The table also lists one identical fastest decay time for
     * the top four q values (60..63), and the chip's clock gate clamps
     * the step there; the register value itself is unchanged, only the
     * increment saturates (SLOPSIX_EG_RATE_MAX).
     *
     * The increment is the amount the envelope's level moves per
     * envelope *step*, and the reference cadence steps the envelope
     * once per 64-sample block: the increment is therefore a block's
     * worth.  The per-sample cadence divides it by the block size, so
     * that the two cadences agree in timing and differ only in how the
     * level is interpolated across the block.
     */
    q = ((int32_t)rate * 41) >> 6;
    q += (int32_t)eg->rate_scaling;
    if (q > SLOPSIX_EG_RATE_MAX) {
        q = SLOPSIX_EG_RATE_MAX;
    }
    inc = (4 + (q & 3)) << (8 + (q >> 2));
    eg->inc = slopsix_eg_scale(inc, eg->sample_rate);

    /* The static count: the measured duration of a stage that does not
     * move the level, and of the attack out of silence. */
    eg->static_count = 0;
    if (((eg->target == eg->level) && (stage >= 2u)) ||
        ((stage == 0u) && (level_param == 0u))) {
        uint32_t static_rate = (uint32_t)rate + (uint32_t)eg->rate_scaling;
        int32_t count;

        if (static_rate > 99u) {
            static_rate = 99u;
        }

        if (static_rate < 77u) {
            count = slopsix_eg_statics[static_rate];
            if ((stage == 0u) && (level_param == 0u)) {
                /* The attack out of silence is twenty times faster. */
                count /= 20;
            }
        } else {
            /* The rates beyond the measured range are approximated. */
            count = 20 * (int32_t)(99u - static_rate);
        }

        eg->static_count = slopsix_eg_scale(count, eg->sample_rate);
    }
}

/*
 * The envelope's life cycle.
 */
void slopsix_eg_init(slopsix_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4], int32_t op_level_q8,
    uint8_t rate_scaling, uint32_t sample_rate)
{
    unsigned i;

    memset(eg, 0, sizeof(*eg));

    for (i = 0; i < 4u; i++) {
        eg->rates[i] = rates[i];
        eg->levels[i] = levels[i];
    }

    eg->op_level_q8 = op_level_q8;
    eg->rate_scaling = rate_scaling;
    eg->sample_rate = sample_rate;
    eg->down = 1u;
    eg->active = 1u;

    slopsix_eg_advance(eg, 0u);
}

void slopsix_eg_note_on(slopsix_eg *eg)
{
    eg->level = 0;
    eg->static_count = 0;
    eg->down = 1u;
    eg->block_pos = 0u;

    slopsix_eg_advance(eg, 0u);
}

void slopsix_eg_note_off(slopsix_eg *eg)
{
    if (eg->down == 0u) {
        return;
    }
    eg->down = 0u;
    eg->static_count = 0;

    /* The key up jumps straight to the release stage. */
    slopsix_eg_advance(eg, 3u);
}

/*
 * One envelope step.
 *
 * The level moves towards the current stage's target: downwards by the
 * stage's increment, upwards along the attack's curve, which jumps to
 * its starting point and then converges on the asymptote.  A stage that
 * does not move the level waits for its measured duration.
 *
 * The number of samples the step covers is one in the per-sample cadence
 * and the block's size in the block cadence; the increment and the static
 * count follow it.
 *
 * The function returns the envelope's level in the Q8 domain.
 */
static int32_t slopsix_eg_advance_level(slopsix_eg *eg, uint32_t samples)
{
    int32_t inc = eg->inc;
    uint8_t expired = 0u;

    if (samples > 1u) {
        /* The block cadence: the increment is a block's worth. */
    } else {
        /* The per-sample cadence: the increment is divided by the block
         * size, so that both cadences agree in timing. */
        inc >>= 6;
    }

    if (eg->static_count > 0) {
        eg->static_count -= (int32_t)samples;
        if (eg->static_count <= 0) {
            eg->static_count = 0;
            slopsix_eg_advance(eg, (uint8_t)(eg->stage + 1u));
            /* The step that ends a hold is the held stage's own last
             * step: like the step that reaches a stage's target, it
             * still belongs to the stage that is ending, so the hold
             * lasts its measured count in full and the next stage's
             * movement starts on the following call. */
            expired = 1u;
        }
    }

    if ((eg->stage < 3u) || ((eg->stage == 3u) && (eg->down == 0u))) {
        if ((eg->static_count == 0) && (expired == 0u)) {
            if (eg->rising) {
                /* The attack: jump to the starting point, then converge
                 * on the asymptote. */
                if (eg->level < (SLOPSIX_EG_JUMP_Q8 << SLOPSIX_EG_FRAC)) {
                    eg->level = SLOPSIX_EG_JUMP_Q8 << SLOPSIX_EG_FRAC;
                }
                eg->level += (((SLOPSIX_EG_MAX_Q8 << SLOPSIX_EG_FRAC) -
                               eg->level) >> 24) * inc;
                if (eg->level >= eg->target) {
                    eg->level = eg->target;
                    slopsix_eg_advance(eg, (uint8_t)(eg->stage + 1u));
                }
            } else {
                eg->level -= inc;
                if (eg->level <= eg->target) {
                    eg->level = eg->target;
                    slopsix_eg_advance(eg, (uint8_t)(eg->stage + 1u));
                }
            }
        }
    }

    return eg->level;
}

int32_t slopsix_eg_step(slopsix_eg *eg)
{
    if (eg->block_mode) {
        /* The block cadence: the envelope advances once per block and
         * the level is interpolated across it (plan decision D-9). */
        int32_t out;

        if (eg->block_pos == 0u) {
            int32_t start = eg->level;
            int32_t end = slopsix_eg_advance_level(eg, SLOPSIX_EG_BLOCK);

            eg->block_level = start;
            eg->block_step = ((end - start) + (1 << 5)) >> 6;
        }

        /* The first sample of the block is one step into the
         * interpolation, the last one is at the block's end. */
        out = (eg->block_level +
               (((int32_t)eg->block_pos + 1) * eg->block_step)) >>
              SLOPSIX_EG_FRAC;

        eg->block_pos =
            (uint8_t)((eg->block_pos + 1u) & (SLOPSIX_EG_BLOCK - 1u));

        return out;
    }

    return slopsix_eg_advance_level(eg, 1u) >> SLOPSIX_EG_FRAC;
}

/*
 * `count` steps in one call: the same arithmetic as `count`
 * slopsix_eg_step calls, in both cadences, leaving the same state.  The
 * block renderer fills an operator's level buffer for the whole
 * sub-block with this and pays the cross-TU call once per operator and
 * sub-block instead of once per sample.
 */
void slopsix_eg_fill(slopsix_eg *eg, int16_t *out, unsigned count)
{
    unsigned s;

    if (eg->block_mode) {
        for (s = 0; s < count; s++) {
            int32_t value;

            if (eg->block_pos == 0u) {
                int32_t start = eg->level;
                int32_t end = slopsix_eg_advance_level(eg, SLOPSIX_EG_BLOCK);

                eg->block_level = start;
                eg->block_step = ((end - start) + (1 << 5)) >> 6;
            }

            value = (eg->block_level +
                     (((int32_t)eg->block_pos + 1) * eg->block_step)) >>
                    SLOPSIX_EG_FRAC;

            out[s] = (int16_t)value;

            eg->block_pos =
                (uint8_t)((eg->block_pos + 1u) & (SLOPSIX_EG_BLOCK - 1u));
        }
        return;
    }

    for (s = 0; s < count; s++) {
        out[s] = (int16_t)(slopsix_eg_advance_level(eg, 1u) >>
                           SLOPSIX_EG_FRAC);
    }
}
