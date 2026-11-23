/*
 * slopsix_eg.c - The amplitude envelope and the level pipeline.
 *
 * The envelope is the EGS model (algorithm page §5.1): the level is the
 * chip's inverted 12-bit attenuation (0 is full level, 0xFF0 the model's
 * silence floor), the stage target composes the stage's 6-bit level
 * register and the operator-volume register in that domain, and the rate
 * gate clocks the movement from a 16-bit master clock that advances at
 * the engine's anchor rate.  The render path works in the opposite
 * (gain) direction: the level pipeline still composes everything in the
 * Q8 log2 domain (256 units per octave) and converts once, at the signal
 * exponent.  The chain is
 *
 *   log_amp = envelope level (Q8)
 *            + carrier-count compensation (Q8, negative)
 *            + amplitude modulation (Phase 4)
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
    return (uint8_t)((slopsix_note_code[(uint8_t)(breakpoint + 20u) & 127u] >> 2)
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
uint8_t slopsix_rate_scaling(uint8_t note, int32_t pitch_offset,
    uint8_t sensitivity)
{
    uint8_t sens = sensitivity > 7u ? 7u : sensitivity;
    int32_t word;
    int32_t x;

    /* The EGS derives the rate-scaling group from the voice-pitch word it
     * receives with the key event, not from the key number: the firmware
     * writes `note word + pitch EG level - bias + master tune`, and the
     * chip forms `clamp((voicePitch >> 8) - 16, 0..27)`.  The neutral pitch
     * EG level is 0x4000 and the firmware's shipped master tune is 0x100,
     * so the constant below is the same word bias the note-only form
     * (`note / 3 - 7`) already reproduced for neutral envelopes.  A pitch
     * EG that starts away from neutral therefore moves the rate-scaling
     * group with it, exactly as the EGS's voice-pitch register does. */
    word = (int32_t)slopsix_note_word[note & 127u] + pitch_offset +
           (SLOPSIX_PITCH_EG_NEUTRAL_WORD - SLOPSIX_FREQ_BIAS + 0x100);
    x = slopsix_floor_div(word, 10) - 16;

    if (x < 0) {
        x = 0;
    }
    if (x > 27) {
        x = 27;
    }

    /* round(x * sens / 7), in qrate units (research E-03). */
    return (uint8_t)(((int32_t)sens * x + 3) / 7);
}

/* ------------------------------------------ the carrier-count compensation */

/* ------------------------------------------------------------ the envelope */

/*
 * The EGS envelope model (algorithm page §5.1).
 *
 * The level is the chip's inverted 12-bit attenuation: 0 is full level,
 * 0xFF0 is the model's silence floor.  The target is composed from the
 * stage's 6-bit attenuation register and the operator-volume register;
 * the stage's rate comes from the 6-bit rate register plus the note's
 * rate scaling.  A 16-bit master clock drives the rate gate: slow rates
 * step once every 2^nshift clocks with a fractional mask, fast rates
 * move by 1<<pshift on the mask's phases.  The stage machine is the
 * EGS's own (S0 -> S1 -> S2 while the key is held, S3 at key-up).
 *
 * The engine's render path works in the opposite (gain) direction, so
 * the exported level is SLOPSIX_EG_GAIN_BASE - attenuation.  The two
 * domains meet at the target composition, where the engine's level
 * pipeline already composes the same quantities.
 */

/* The fractional-qrate clock masks: bit 0 is the clock's phase 0.  A set
 * bit lets the envelope step on that phase of the eight-clock cycle. */
static const uint8_t slopsix_eg_outmask[4] = {
    0xAA, 0xEA, 0xEE, 0xFE
};

/* The envelope's level in the render path's gain direction, in the Q8
 * domain with the block cadence's fractional bits still attached. */
static int32_t slopsix_eg_gain_q8(const slopsix_eg *eg)
{
    return ((int32_t)SLOPSIX_EG_GAIN_BASE << SLOPSIX_EG_FRAC) - eg->level;
}

static int32_t slopsix_eg_gain_value(const slopsix_eg *eg)
{
    return slopsix_floor_div(slopsix_eg_gain_q8(eg), SLOPSIX_EG_FRAC);
}

/* The EGS's pre-quantized registers.  The rate register is the firmware's
 * (rate * 164) >> 8, 0..63; the level register is TABLE_LOG[level] >> 1,
 * which is the exact inverse of the engine's gain curve. */
static uint8_t slopsix_eg_rate_reg(uint8_t rate)
{
    if (rate > 99u) {
        rate = 99u;
    }
    return (uint8_t)(((unsigned)rate * 164u) >> 8);
}

static uint8_t slopsix_eg_level_reg(uint8_t level)
{
    if (level > 99u) {
        level = 99u;
    }
    return (uint8_t)(63u - ((unsigned)slopsix_level_curve(level) >> 1));
}

/* The operator-volume register from the engine's Q8 operator level.  The
 * firmware clamps it to at least SLOPSIX_EG_OP_LEVEL_MIN. */
static uint8_t slopsix_eg_op_level(int32_t op_level_q8)
{
    int32_t value = slopsix_floor_div(
        SLOPSIX_LEVEL_OFFSET_Q8 - op_level_q8, 4);

    if (value < SLOPSIX_EG_OP_LEVEL_MIN) {
        value = SLOPSIX_EG_OP_LEVEL_MIN;
    }
    if (value > 255) {
        value = 255;
    }
    return (uint8_t)value;
}

/* The current attenuation, as the stage walk sees it. */
static int32_t slopsix_eg_att(const slopsix_eg *eg)
{
    return eg->level >> SLOPSIX_EG_FRAC;
}

/* Set the stage's target, its delay compression and the attack direction. */
static void slopsix_eg_update_target(slopsix_eg *eg)
{
    uint8_t stage = eg->stage;
    int32_t target;

    target = ((int32_t)eg->level_reg[stage] << 6) +
             ((int32_t)eg->op_level << 4);
    if (target > SLOPSIX_EG_ATT_MAX) {
        target = SLOPSIX_EG_ATT_MAX;
    }

    /* The delay compression: a stage 0/1 whose current and previous
     * levels are both at or beyond 40 (the level parameter is below 20)
     * is delayed.  Stage 0's delay is compressed when the previous
     * stage-0 delay did not complete. */
    if (stage < 2u) {
        uint8_t prev = (uint8_t)((stage + 3u) & 3u);

        if (eg->level_reg[stage] >= 40u && eg->level_reg[prev] >= 40u) {
            int32_t delay = (stage == 0u && eg->compress) ? 15 : 479;
            int32_t level = slopsix_eg_att(eg);

            target = level + delay;
            if (target > SLOPSIX_EG_ATT_MAX) {
                eg->level = (SLOPSIX_EG_ATT_MAX - delay) <<
                            SLOPSIX_EG_FRAC;
                target = SLOPSIX_EG_ATT_MAX;
            }
        }
    }

    eg->target = target << SLOPSIX_EG_FRAC;
    eg->rising = (target < slopsix_eg_att(eg)) ? 1u : 0u;
}

/* Set the stage's clock gate. */
static void slopsix_eg_update_rate(slopsix_eg *eg)
{
    unsigned qrate = (unsigned)eg->rate_reg[eg->stage] +
                     (unsigned)eg->rate_scaling;

    if (qrate > 63u) {
        qrate = 63u;
    }

    eg->pshift = 0u;
    eg->nshift = 0u;
    eg->small = 0u;
    if (qrate < 44u) {
        eg->nshift = (uint8_t)(11u - (qrate >> 2));
        eg->small = (uint16_t)((1u << eg->nshift) - 1u);
    } else {
        eg->pshift = (uint8_t)((qrate >> 2) - 11u);
    }
    eg->mask = slopsix_eg_outmask[qrate & 3u];
}

/* Advance the stage machine.  A key-up jumps to the release stage; a
 * key-on starts again at stage 0. */
static void slopsix_eg_advance(slopsix_eg *eg)
{
    switch (eg->stage) {
    case 0u:
        if (eg->down == 0u) {
            eg->stage = 3u;
            eg->compress = 1u;
        } else {
            eg->stage = 1u;
            eg->compress = 0u;
        }
        break;
    case 1u:
        if (eg->down == 0u) {
            eg->stage = 3u;
        } else {
            eg->stage = 2u;
        }
        break;
    case 2u:
        if (eg->down == 0u) {
            eg->stage = 3u;
        } else {
            return;
        }
        break;
    case 3u:
        if (eg->down != 0u) {
            eg->stage = 0u;
        } else {
            return;
        }
        break;
    default:
        return;
    }

    slopsix_eg_update_target(eg);
    slopsix_eg_update_rate(eg);
}

/* The EGS master clock's tick threshold: the 16-bit clock advances at the
 * engine's anchor rate (research E-02), whatever the host's rate, so the
 * envelope's durations are real-time quantities (research F-04).  The
 * threshold is the host rate itself: the clock's sub-tick accumulator
 * adds the anchor per host sample and a tick fires each time it crosses
 * the threshold, so `threshold == anchor` ticks once per sample and other
 * rates dither the schedule.  The rate is bounded like the cadence gates'
 * and the frequency path's (zero means the anchor; at most eight times,
 * and at least a 64th of the anchor so one sample cannot owe too many
 * ticks). */
uint32_t slopsix_eg_tick_threshold(uint32_t sample_rate)
{
    uint32_t rate = sample_rate;

    if (rate == 0u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    }
    if (rate > SLOPSIX_DEFAULT_SAMPLE_RATE * 8u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE * 8u;
    }
    if (rate < SLOPSIX_DEFAULT_SAMPLE_RATE / 64u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE / 64u;
    }

    return rate;
}

/* The gate's body, shared by the two ladders below: the gate reads the
 * clock's pre-increment value, the movement happens on the phases the
 * rate selects, and the walk advances when the level reaches the target.
 * The clock and the threshold live in the caller's registers; the body
 * writes the level and may advance the stage. */
#define SLOPSIX_EG_TICK_BODY           \
    do {                               \
        if (!(eg->stage > 1u && eg->level == eg->target) &&                 \
            ((clock & eg->small) == 0u) &&                                  \
            ((eg->mask & (1u << ((clock >> eg->nshift) & 7u))) != 0u)) {    \
            int32_t level = slopsix_eg_att(eg);                             \
            int32_t target = eg->target >> SLOPSIX_EG_FRAC;                 \
            uint8_t reached = 0u;                                           \
            if (eg->rising) {                                               \
                if (level > (int32_t)SLOPSIX_EG_ATT_JUMP) {                 \
                    level = (int32_t)SLOPSIX_EG_ATT_JUMP;                   \
                }                                                           \
                level -= ((level >> 8) + 2) << eg->pshift;                  \
                if (level <= target) {                                      \
                    level = target;                                         \
                    reached = 1u;                                           \
                }                                                           \
            } else {                                                        \
                level += 1 << eg->pshift;                                   \
                if (level >= target) {                                      \
                    level = target;                                         \
                    reached = 1u;                                           \
                }                                                           \
            }                                                               \
            if (level < 0) {                                                \
                level = 0;                                                  \
            }                                                               \
            if (level > (int32_t)SLOPSIX_EG_ATT_MAX) {                      \
                level = (int32_t)SLOPSIX_EG_ATT_MAX;                        \
            }                                                               \
            eg->level = level << SLOPSIX_EG_FRAC;                           \
            if (reached) {                                                  \
                slopsix_eg_advance(eg);                                     \
            }                                                               \
        }                                                                   \
    } while (0)

/* The anchor's own ladder: one tick per host sample, no accumulator (the
 * common host, and the only host the hardware itself runs at). */
static void slopsix_eg_clock_1(slopsix_eg *eg)
{
    uint16_t clock;

    if (eg->stage > 3u) {
        return;
    }

    clock = eg->clock;
    SLOPSIX_EG_TICK_BODY;
    eg->clock = (uint16_t)(clock + 1u);
}

/* The dithered ladder: the clock advances at the anchor rate; each tick
 * evaluates the gate once - the gate's phases and its fractional masks
 * are the clock's own - and the stage's movement happens on the phases
 * the rate selects.  At rates above the anchor one sample owes at most
 * one tick; below it, the loop runs for the ticks the sample owes. */
static void slopsix_eg_clock_d(slopsix_eg *eg)
{
    uint32_t threshold;
    uint32_t acc;
    uint16_t clock;

    if (eg->stage > 3u) {
        return;
    }

    threshold = eg->tick_threshold;
    acc = eg->tick_acc + SLOPSIX_DEFAULT_SAMPLE_RATE;
    clock = eg->clock;

    while (acc >= threshold) {
        SLOPSIX_EG_TICK_BODY;
        clock = (uint16_t)(clock + 1u);
        acc -= threshold;
    }

    eg->clock = clock;
    eg->tick_acc = acc;
}

/* One step of the master clock: the anchor's ladder without the
 * accumulator, the dithered ladder otherwise.  The selection is one
 * predictable compare; the envelope's durations are real-time quantities
 * at every host rate (research F-04). */
static void slopsix_eg_clock(slopsix_eg *eg)
{
    if (eg->tick_threshold == SLOPSIX_DEFAULT_SAMPLE_RATE) {
        slopsix_eg_clock_1(eg);
    } else {
        slopsix_eg_clock_d(eg);
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
        eg->rate_reg[i] = slopsix_eg_rate_reg(rates[i]);
        eg->level_reg[i] = slopsix_eg_level_reg(levels[i]);
    }

    eg->op_level = slopsix_eg_op_level(op_level_q8);
    eg->rate_scaling = rate_scaling;
    eg->tick_threshold = slopsix_eg_tick_threshold(sample_rate);
    /* A fresh envelope counts as one whose previous stage-0 delay did
     * not complete (the model's own default: the first trigger takes the
     * compressed stage-0 delay). */
    eg->compress = 1u;
    eg->active = 1u;

    slopsix_eg_note_on(eg);
}

void slopsix_eg_note_on(slopsix_eg *eg)
{
    /* A full trigger starts from the hardware's silence floor; the
     * key-on transition then enters stage 0.  The compress flag keeps
     * whatever the envelope's previous walk left: the stage machine
     * tracks whether the previous stage-0 delay completed (a stage-0
     * key-off sets it, a completed stage 0 clears it), exactly as the
     * model does (algorithm page §5.1). */
    eg->level = (int32_t)SLOPSIX_EG_ATT_MAX << SLOPSIX_EG_FRAC;
    eg->target = eg->level;
    eg->stage = 3u;
    eg->down = 1u;
    eg->clock = 0u;
    eg->tick_acc = 0u;
    eg->rising = 0u;
    eg->block_pos = 0u;
    eg->block_level = eg->level;
    eg->block_step = 0;

    slopsix_eg_advance(eg);
}

void slopsix_eg_note_off(slopsix_eg *eg)
{
    if (eg->down == 0u) {
        return;
    }
    eg->down = 0u;
    slopsix_eg_advance(eg);
}

void slopsix_eg_silence(slopsix_eg *eg)
{
    /* The firmware's VOICE_RESET_EGS drives every operator level to its
     * maximum attenuation and keys the voice off.  Its key event walk
     * (off, on, off) leaves the stage-0 delay compressed, as a stage-0
     * key-off does; stage 4 holds the floor without moving. */
    eg->level = (int32_t)SLOPSIX_EG_ATT_MAX << SLOPSIX_EG_FRAC;
    eg->target = eg->level;
    eg->stage = 4u;
    eg->active = 1u;
    eg->rising = 0u;
    eg->down = 0u;
    eg->compress = 1u;
    eg->block_pos = 0u;
    eg->block_level = eg->level;
    eg->block_step = 0;
}

/*
 * One envelope step.  The per-sample cadence advances the EGS master
 * clock once; the diagnostic block cadence advances it across a block
 * and interpolates the level between the block's endpoints.
 */
int32_t slopsix_eg_step(slopsix_eg *eg)
{
    if (eg->block_mode) {
        int32_t out;

        if (eg->block_pos == 0u) {
            int32_t start = eg->level;
            int32_t end;
            unsigned i;

            for (i = 0; i < SLOPSIX_EG_BLOCK; i++) {
                slopsix_eg_clock(eg);
            }
            end = eg->level;
            eg->block_level = start;
            eg->block_step = slopsix_floor_div(end - start + (1 << 5), 6);
        }

        out = slopsix_floor_div(
            ((int32_t)SLOPSIX_EG_GAIN_BASE << SLOPSIX_EG_FRAC) -
                (eg->block_level +
                 (((int32_t)eg->block_pos + 1) * eg->block_step)),
            SLOPSIX_EG_FRAC);

        eg->block_pos =
            (uint8_t)((eg->block_pos + 1u) & (SLOPSIX_EG_BLOCK - 1u));

        return out;
    }

    /* The per-sample walk: one clock call per sample, the ladder chosen
     * here rather than in a wrapper (the render path's walk loop). */
    if (eg->tick_threshold == SLOPSIX_DEFAULT_SAMPLE_RATE) {
        slopsix_eg_clock_1(eg);
    } else {
        slopsix_eg_clock_d(eg);
    }
    return slopsix_eg_gain_value(eg);
}

/*
 * `count` steps in one call: the same arithmetic as `count`
 * slopsix_eg_step calls, in both cadences, leaving the same state.  The
 * block renderer fills an operator's level buffer for the whole
 * sub-block with this and pays the cross-TU call once per operator and
 * sub-block instead of once per sample.
 *
 * The per-sample loop selects the clock ladder once, outside the loop:
 * the anchor's ladder is the common host's, and the dithered ladder is
 * the one that pays the sub-tick accounting (docs/performance.md's
 * benchmark runs the render path in this loop).
 */
void slopsix_eg_fill(slopsix_eg *eg, int16_t *out, unsigned count)
{
    unsigned s;

    if (eg->block_mode) {
        for (s = 0; s < count; s++) {
            int32_t value;

            if (eg->block_pos == 0u) {
                int32_t start = eg->level;
                int32_t end;
                unsigned i;

                for (i = 0; i < SLOPSIX_EG_BLOCK; i++) {
                    slopsix_eg_clock(eg);
                }
                end = eg->level;
                eg->block_level = start;
                eg->block_step = slopsix_floor_div(end - start + (1 << 5), 6);
            }

            value = slopsix_floor_div(
                ((int32_t)SLOPSIX_EG_GAIN_BASE << SLOPSIX_EG_FRAC) -
                    (eg->block_level +
                     (((int32_t)eg->block_pos + 1) * eg->block_step)),
                SLOPSIX_EG_FRAC);

            out[s] = (int16_t)value;

            eg->block_pos =
                (uint8_t)((eg->block_pos + 1u) & (SLOPSIX_EG_BLOCK - 1u));
        }
        return;
    }

    if (eg->tick_threshold == SLOPSIX_DEFAULT_SAMPLE_RATE) {
        for (s = 0; s < count; s++) {
            slopsix_eg_clock_1(eg);
            out[s] = (int16_t)slopsix_eg_gain_value(eg);
        }
        return;
    }

    for (s = 0; s < count; s++) {
        slopsix_eg_clock_d(eg);
        out[s] = (int16_t)slopsix_eg_gain_value(eg);
    }
}
