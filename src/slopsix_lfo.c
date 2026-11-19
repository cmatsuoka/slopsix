/*
 * slopsix_lfo.c - The LFO engine and the two EGS modulation registers it
 * feeds: the speed/delay laws, the six waveforms, the per-tick
 * compositions of the amp-mod register and the pitch-modification word,
 * and the controller arithmetic that feeds both (research fw
 * MOD_PROCESS_INPUT_SOURCES).
 *
 * The LFO is CPU-side and global to the instrument (research E-04,
 * L-01/L-02/L-03/L-06): the hardware's interrupt handler advances it once
 * per OCF tick and pushes both registers on the same tick, so the two
 * register staircases step at about 375.3 Hz.  The engine keeps that
 * staircase; it does not interpolate per sample.  The update cadence is
 * not in this file: the gate lives with the context
 * (src/slopsix_internal.h) and this file performs one tick's work, so a
 * tick cannot be applied twice.
 *
 * Unit domains (plan decision D-12):
 *
 *   - The pitch-modification word is the EGS register's own 16-bit word
 *     in units of 1/16384 of an octave, the same word and the same units
 *     the bend parser produces; the firmware composes the wave's term and
 *     the bend into one word with a wrapping 16-bit add, whose wrap is
 *     the word's only saturation.
 *   - The engine's log-frequency word is the firmware's 4096-per-octave
 *     word, so the composed word enters it shifted right by two (floor
 *     division); both quantities denote the same pitch, and the shift is
 *     the conversion between the chips' domains.
 *   - The amp-mod register's word is the EGS's 12-bit attenuation domain
 *     whose unit is the same 1/256-octave step as the engine's Q8 unit
 *     with the opposite sign; the operator walk subtracts the value times
 *     the sensitivity shift.
 *   - The LFO parameters keep their hardware domains: the depths are the
 *     shared 660-unit value scale's high bytes, the sensitivity is the
 *     ROM table's value, and the wave amplitude is the signed 8-bit byte.
 *
 * The waveform, delay and register recipes follow the firmware's own
 * routines instruction for instruction, so the hardware's quirks survive
 * (the algorithm page §§4.5, 6.5, 6.6); the three tables at the bottom
 * are the ROM's, verbatim.
 *
 * The DX7S per-voice LFO (src/dx7s/slopsix_lfo.c) reuses these recipes:
 * its per-voice tick calls the same wave and register steps with the
 * voice's own state.  Nothing here is duplicated.
 *
 * This translation unit runs at control rate, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* ------------------------------------------------- the activation scaling */

/*
 * The speed law (the shared value scale's high byte times its own
 * multiplier), with the zero speed's accumulator seed.  The firmware
 * clamps the zero input to one and multiplies it by the seed, which is
 * the same 11.
 */
uint16_t slopsix_lfo_speed_increment(uint8_t speed)
{
    uint32_t high;
    uint32_t mult;

    if (speed == 0u) {
        return 11u;
    }
    if (speed > 99u) {
        speed = 99u;
    }

    high = (660u * speed) >> 8;
    if (high < 160u) {
        mult = 11u;
    } else {
        mult = 11u + ((high - 160u) >> 2);
    }

    return (uint16_t)(high * mult);
}

/*
 * The delay law: the delay's counter increment per OCF tick, from the
 * 99-minus-delay value's low nibble and group.
 */
uint16_t slopsix_lfo_delay_increment(uint8_t delay)
{
    uint32_t a = 99u - delay;    /* the caller clamps the delay to 0..99 */
    uint32_t u = a & 15u;
    uint32_t k = a >> 4;

    return (uint16_t)((16u + u) << (k + 2u));
}

/*
 * The parameter half of the patch activation: the dump's bytes 137..143
 * through the firmware's own activation (research fw PATCH_ACTIVATE_LFO).
 * The shared DX7 path calls this for the context-wide LFO; the DX7S calls
 * it for every voice.  Anything larger than a byte's own range is clamped
 * here, once, as the other dump fields are.
 */
void slopsix_lfo_activate(slopsix_lfo *lfo, const uint8_t *dump)
{
    uint8_t speed = dump[SLOPSIX_DUMP_LFO_SPEED];
    uint8_t delay = dump[SLOPSIX_DUMP_LFO_DELAY];
    uint8_t depth;

    if (speed > 99u) {
        speed = 99u;
    }
    if (delay > 99u) {
        delay = 99u;
    }

    lfo->phase_increment = slopsix_lfo_speed_increment(speed);
    lfo->delay_increment = slopsix_lfo_delay_increment(delay);

    /* The two depths are the shared 660-unit value scale's high bytes
     * (0..255 at parameter 99). */
    depth = dump[SLOPSIX_DUMP_LFO_PMD];
    lfo->pitch_mod_depth = (uint8_t)((((uint32_t)660u *
        (depth > 99u ? 99u : depth))) >> 8);
    depth = dump[SLOPSIX_DUMP_LFO_AMD];
    lfo->amp_mod_depth = (uint8_t)((((uint32_t)660u *
        (depth > 99u ? 99u : depth))) >> 8);

    /* The waveform's byte is stored raw: a value outside the wave enums
     * is the S&H's fall-through on the render path, exactly as the
     * hardware reads it. */
    lfo->waveform = dump[SLOPSIX_DUMP_LFO_WAVE];

    /* The sensitivity indexes the ROM's table; an out-of-contract dump
     * byte clamps to the table's last entry. */
    {
        unsigned sens = dump[SLOPSIX_DUMP_LFO_PMS];

        if (sens > 7u) {
            sens = 7u;
        }
        lfo->pitch_mod_sens = slopsix_pitch_mod_sens[sens];
    }
}

void slopsix_lfo_patch_activate(slopsix_context_state *state,
    const uint8_t *dump)
{
    uint8_t delay = dump[SLOPSIX_DUMP_LFO_DELAY];

    slopsix_lfo_activate(&state->lfo, dump);

    /* The note-on path's own bytes. */
    if (delay > 99u) {
        delay = 99u;
    }
    state->patch.lfo_delay = delay;
    state->patch.lfo_sync = dump[SLOPSIX_DUMP_LFO_SYNC] != 0u;
}

/* ------------------------------------------------------- the controllers */

/*
 * An 8-bit saturating add, as the hardware's add-and-clamp pairs do.
 */
static inline unsigned slopsix_add_clamp8(unsigned a, unsigned b)
{
    unsigned sum = a + b;

    return sum > 0xFFu ? 0xFFu : sum;
}

/*
 * The controller arithmetic (research fw MOD_PROCESS_INPUT_SOURCES, ROM
 * $E208): the four sources' scaled inputs, the amplitude-modulation
 * factor and the EG-bias total, in the firmware's own order.  The caller
 * recomputes it when a controller value, range or routing changes and at
 * reset - the firmware runs it on the analog-input and function-parameter
 * events - and never on the sample path.
 *
 * The scaled input is the MUL pair's high byte: the analog input
 * (0..127, the same units the firmware's MIDI handlers store) times the
 * range's scaled high byte, (660*range) >> 8 of the shared value scale.
 * The amp-routed sources' scaled inputs clamp-sum into the
 * amplitude-modulation factor.
 *
 * The EG-bias total is inverted polarity.  The EG-routed ranges' scaled
 * values first clamp-sum into the range total, which the firmware
 * complements once after the sources' loop; the EG-routed inputs' clamped
 * sum is then added to that complement, clamped again, and the result is
 * complemented into the total the amp-mod register's composition
 * consumes.  With inputs of 0..127 the scaled input never exceeds its
 * range's high byte, so the arithmetic holds no wrap and the total is the
 * range's sum minus the inputs' sum: nothing routed leaves it at 0 (the
 * composition's neutral), and a routed, untouched controller pushes the
 * range's own sum as the resting bias.
 */
void slopsix_mod_process_input_sources(slopsix_context_state *state)
{
    unsigned scaled_range_total = 0u;
    unsigned scaled_input_sum = 0u;
    unsigned amp_factor = 0u;
    unsigned i;

    for (i = 0; i < SLOPSIX_CTRL_SRC_COUNT; i++) {
        unsigned scaled = (660u * (unsigned)state->ctrl_range[i]) >> 8;

        if ((state->ctrl_assign[i] & SLOPSIX_ASSIGN_EG) != 0u) {
            scaled_range_total = slopsix_add_clamp8(scaled_range_total,
                scaled);
        }
        state->ctrl_scaled_input[i] =
            (uint8_t)((scaled * (unsigned)state->ctrl_value[i]) >> 8);
    }
    scaled_range_total = ~scaled_range_total & 0xFFu;

    for (i = 0; i < SLOPSIX_CTRL_SRC_COUNT; i++) {
        if ((state->ctrl_assign[i] & SLOPSIX_ASSIGN_AMP) != 0u) {
            amp_factor = slopsix_add_clamp8(amp_factor,
                state->ctrl_scaled_input[i]);
        }
        if ((state->ctrl_assign[i] & SLOPSIX_ASSIGN_EG) != 0u) {
            scaled_input_sum = slopsix_add_clamp8(scaled_input_sum,
                state->ctrl_scaled_input[i]);
        }
    }

    state->mod_amp_factor = (uint8_t)amp_factor;
    state->mod_eg_bias_inv = (uint8_t)(~slopsix_add_clamp8(scaled_input_sum,
        scaled_range_total) & 0xFFu);
}

/* ----------------------------------------------------- the per-tick work */

/*
 * Compute the wave amplitude for the current phase.
 *
 * The recipes follow the firmware's own selection chain: the triangle
 * shifts the 16-bit accumulator left and its saved high byte decides the
 * vertical inversion, the saws use the high byte directly (down as its
 * complement), the square's halves follow the accumulator's top bit, the
 * sine walks the 64-entry table mirrored and inverted, and the
 * sample-and-hold keeps its value - resampling by its linear
 * congruential step only on the ticks whose phase addition overflows the
 * accumulator's signed range (a positive phase crossing 0x7FFF into
 * 0x8000).  Any waveform value the selection chain does not name falls
 * through to the sample-and-hold, exactly as the hardware reads it.
 */
void slopsix_lfo_wave_step(slopsix_lfo *lfo, unsigned sh_flag)
{
    uint16_t phase = lfo->phase;
    unsigned high = phase >> 8;

    switch (lfo->waveform) {
    case 0u:
        /* The triangle: the 16-bit shift's high byte drops the
         * accumulator's own top bit; the saved high byte (whose bit 7 is
         * the accumulator's bit 15) selects the inversion. */
        {
            unsigned value = (phase & 0x7FFFu) >> 7;

            if ((high & 0x80u) != 0u) {
                value = 0xFFu - value;
            }
            lfo->amplitude = (int8_t)(uint8_t)(value + 0x80u);
        }
        return;
    case 1u:
        /* The falling saw: the high byte's complement. */
        lfo->amplitude = (int8_t)(uint8_t)(0xFFu - high);
        return;
    case 2u:
        /* The rising saw: the high byte as a signed byte. */
        lfo->amplitude = (int8_t)(uint8_t)high;
        return;
    case 3u:
        /* The square: the phase's first half is the positive half. */
        lfo->amplitude = (high & 0x80u) != 0u ? (int8_t)0x80 : (int8_t)0x7F;
        return;
    case 4u:
        /* The sine: the 64-entry table, mirrored and inverted. */
        {
            unsigned index = high & 63u;
            unsigned value;

            if ((high & 64u) != 0u) {
                index ^= 63u;
            }
            value = slopsix_lfo_sin_table[index];
            if ((high & 0x80u) != 0u) {
                value = 0xFFu - value;
            }
            lfo->amplitude = (int8_t)(uint8_t)value;
        }
        return;
    default:
        /* Wave 5 and the out-of-range fall-through: sample and hold. */
        break;
    }

    if (sh_flag != 0u) {
        /* The holder's linear congruential step: the multiply's low byte
         * plus the constant, with the byte's wrap discarding anything
         * above it. */
        unsigned acc = (unsigned)lfo->sh_acc * 179u + 11u;

        lfo->sh_acc = (uint8_t)(acc & 0xFFu);
        lfo->amplitude = (int8_t)(uint8_t)lfo->sh_acc;
    }
    /* Without the flag the holder keeps its value. */
}

/*
 * One LFO tick: the hardware's per-tick routine (algorithm page §§4.5,
 * 6.5, 6.6).
 *
 * The delay accumulator advances and locks at its ceiling once it
 * overflows; from then on every tick grows the fade-in factor.  The phase
 * accumulator advances unconditionally (its signed-overflow flag is the
 * sample-and-hold's resample trigger), the wave amplitude is computed,
 * and the two registers' values are composed from the cached controller
 * factors: the amp-routed sources' clamped sum joins the amp-mod factor,
 * and the pitch-routed sources' clamped sum joins the pitch factor - the
 * latter added here, where the firmware's per-tick MOD_PITCH_LOAD_TO_EGS
 * adds it.  The register changes themselves are only the compositions'
 * outputs; the controller factors arrive through the recomposition the
 * setters run (slopsix_mod_process_input_sources).
 */
void slopsix_lfo_tick_one(slopsix_context_state *state, slopsix_lfo *lfo)
{
    uint16_t delay_sum = (uint16_t)((uint32_t)lfo->delay_acc +
                                    lfo->delay_increment);
    uint16_t phase_sum;
    unsigned sh_flag;
    unsigned amp_inv;
    unsigned factor;

    if (delay_sum < lfo->delay_acc) {
        /* The 16-bit add overflowed: the accumulator locks at its
         * ceiling, and the fade-in factor grows by the increment's high
         * byte (at least one) per tick, clamped at $FF. */
        uint32_t step = lfo->delay_increment >> 8;

        if (step == 0u) {
            step = 1u;
        }
        lfo->fade = (uint8_t)slopsix_add_clamp8(lfo->fade, (unsigned)step);
        delay_sum = 0xFFFFu;
    }
    lfo->delay_acc = delay_sum;

    phase_sum = (uint16_t)((uint32_t)lfo->phase + lfo->phase_increment);
    /* The signed addition's overflow flag: the phase and the result
     * differ in sign while the increment's sign matches the result's.
     * The increment is always well below the sign bit, so this is a
     * positive phase crossing into the negative half. */
    sh_flag = ((lfo->phase ^ phase_sum) &
               (lfo->phase_increment ^ phase_sum)) & 0x8000u;
    lfo->phase = phase_sum;

    slopsix_lfo_wave_step(lfo, sh_flag);

    /* The amp-mod register (algorithm page §4.5).  The amplitude
     * factor: the LFO's depth term plus the amp-routed sources' clamped
     * sum.  The EG-bias total folds in in its inverted domain (research
     * fw MOD_AMP_LOAD_TO_EGS): the add/clamp/subtract-back pair before
     * the multiply - the only one of its folds that can leave an effect,
     * and then only past the byte's range - and the plain add/clamp after
     * it.  Nothing routed leaves the total at 0, so both folds are no-ops
     * and the register is the LFO's own composition. */
    factor = ((unsigned)lfo->amp_mod_depth * lfo->fade) >> 8;
    {
        unsigned bias_inv = state->mod_eg_bias_inv;
        unsigned value = slopsix_add_clamp8(factor, state->mod_amp_factor);
        int32_t d;

        value = (slopsix_add_clamp8(value, bias_inv) - bias_inv) & 0xFFu;

        /* The inverted-polarity wave form: the register amount runs with
         * 127 minus the wave's signed byte, so the negative half
         * attenuates more. */
        amp_inv = (unsigned)(127 - (int32_t)lfo->amplitude);
        d = (int32_t)(value * amp_inv) >> 8;

        lfo->amp_mod_register = slopsix_amp_mod_table[0xFFu -
            (unsigned)slopsix_add_clamp8((unsigned)d, bias_inv)];
    }

    /* The pitch-modification word (algorithm page §6.5): the factor and
     * the sensitivity-scaled amplitude, times one byte of each other,
     * right by one. */
    slopsix_mod_pitch_term_update_one(state, lfo);

    /* The register's word: the wave's term plus the current bend,
     * wrapped at 16 bits. */
    lfo->pitch_mod_word = slopsix_mod_pitch_word_one(state, lfo);
}

void slopsix_lfo_tick(slopsix_context_state *state)
{
    slopsix_lfo_tick_one(state, &state->lfo);
}

/* --------------------------------------------------- the word conversions */

/*
 * The pitch-modification register's wave term, recomposed for a context:
 * the factor is the pitch-routed sources' clamped sum added to the LFO's
 * own depth term (the firmware's per-tick MOD_PITCH_LOAD_TO_EGS adds it
 * before the multiply), times one byte of the sensitivity-scaled
 * amplitude, right by one.  The tick runs this with the fresh amplitude;
 * the controller setters run it with the last tick's, so a controller
 * change is audible without waiting for the next tick.
 */
void slopsix_mod_pitch_term_update_one(slopsix_context_state *state,
    slopsix_lfo *lfo)
{
    unsigned factor;
    int32_t magnitude;
    int32_t h;
    unsigned i;

    factor = ((unsigned)lfo->pitch_mod_depth * lfo->fade) >> 8;
    for (i = 0; i < SLOPSIX_CTRL_SRC_COUNT; i++) {
        if ((state->ctrl_assign[i] & SLOPSIX_ASSIGN_PITCH) != 0u) {
            factor = slopsix_add_clamp8(factor,
                state->ctrl_scaled_input[i]);
        }
    }

    magnitude = (int32_t)lfo->pitch_mod_sens *
        (lfo->amplitude < 0 ? -(int32_t)lfo->amplitude
                            : (int32_t)lfo->amplitude);
    h = magnitude >> 8;
    if (lfo->amplitude < 0) {
        h = -h;
    }
    lfo->pitch_mod_term = (int16_t)slopsix_floor_div((int32_t)factor * h, 1);
}

void slopsix_mod_pitch_term_update(slopsix_context_state *state)
{
    slopsix_mod_pitch_term_update_one(state, &state->lfo);
}

/*
 * The pitch-modification register's word for a context: the LFO's
 * per-tick share plus the current bend, wrapped at 16 bits - the
 * firmware's single 16-bit register word.  Both setters that change the
 * bend recompute it, so a bend change is audible without waiting for the
 * next tick; the controller setters recompose the term beside the bend
 * the same way.
 */
int16_t slopsix_mod_pitch_word_one(const slopsix_context_state *state,
    const slopsix_lfo *lfo)
{
    int32_t bend = slopsix_bend_pitch_mod(state->pitch_bend,
                                          state->bend_range, state->bend_step);

    return (int16_t)(uint16_t)((uint16_t)lfo->pitch_mod_term +
                               (uint16_t)bend);
}

int16_t slopsix_mod_pitch_word(const slopsix_context_state *state)
{
    return slopsix_mod_pitch_word_one(state, &state->lfo);
}

/*
 * The composed register word's contribution to the voice's log-frequency
 * word: the 1/16384-octave unit shifted right by two into the engine's
 * 4096-per-octave word, floor division so the half-quantum keeps its
 * direction (the same conversion the bend's word alone takes).
 */
int32_t slopsix_mod_pitch_offset(const slopsix_context_state *state)
{
    return slopsix_floor_div((int32_t)state->lfo.pitch_mod_word,
                             SLOPSIX_PITCH_MOD_TO_LOG_SHIFT);
}

/* --------------------------------------------------------- life cycle */

/*
 * The note-on hook (the firmware's VOICE_ADD side, both the poly and the
 * mono paths): a patch delay that is not 0 clears the delay accumulator
 * and the fade factor, and a patch with LFO SYNC set seeds the phase
 * accumulator to 0x7FFF, just before the triangle's peak.  A delay of 0
 * resets neither, and the LFO state is global, so every note-on runs
 * this.
 */
void slopsix_lfo_note_on_one(slopsix_context_state *state, slopsix_lfo *lfo)
{
    if (state->patch.lfo_delay != 0u) {
        lfo->delay_acc = 0u;
        lfo->fade = 0u;
    }
    if (state->patch.lfo_sync != 0u) {
        lfo->phase = 0x7FFFu;
    }
}

void slopsix_lfo_note_on(slopsix_context_state *state)
{
    slopsix_lfo_note_on_one(state, &state->lfo);
}

/* ------------------------------------------------------------ ROM tables */

/* The sine wave's 64-entry quarter table (ROM TABLE_LFO_SIN, verbatim);
 * the wave walks it mirrored and inverts the output on the negative
 * half. */
const uint8_t slopsix_lfo_sin_table[64] = {
    0x02, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14, 0x17,
    0x1A, 0x1D, 0x20, 0x23, 0x26, 0x29, 0x2C, 0x2F,
    0x32, 0x35, 0x38, 0x3A, 0x3D, 0x40, 0x43, 0x45,
    0x48, 0x4A, 0x4D, 0x4F, 0x52, 0x54, 0x56, 0x59,
    0x5B, 0x5D, 0x5F, 0x61, 0x63, 0x65, 0x67, 0x69,
    0x6A, 0x6C, 0x6E, 0x6F, 0x71, 0x72, 0x73, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7C,
    0x7D, 0x7D, 0x7E, 0x7E, 0x7F, 0x7F, 0x7F, 0x7F
};

/* The amp-modulation register's reduction curve (ROM TABLE_AMP_MOD,
 * verbatim): indexed by the one's complement of the composed factor, so
 * index 255 (factor 0) is no modulation and index 0 (full factor) the
 * maximum attenuation, shaped in between. */
const uint8_t slopsix_amp_mod_table[256] = {
    0xFF, 0xFF, 0xE0, 0xCD, 0xC0, 0xB5, 0xAD, 0xA6,
    0xA0, 0x9A, 0x95, 0x91, 0x8D, 0x89, 0x86, 0x82,
    0x80, 0x7D, 0x7A, 0x78, 0x75, 0x73, 0x71, 0x6F,
    0x6D, 0x6B, 0x69, 0x67, 0x66, 0x64, 0x62, 0x61,
    0x60, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58, 0x56,
    0x55, 0x54, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E,
    0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x47, 0x46,
    0x46, 0x45, 0x44, 0x43, 0x42, 0x42, 0x41, 0x40,
    0x40, 0x3F, 0x3E, 0x3D, 0x3D, 0x3C, 0x3B, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x38, 0x37, 0x36, 0x36,
    0x35, 0x35, 0x34, 0x33, 0x33, 0x32, 0x32, 0x31,
    0x31, 0x30, 0x30, 0x2F, 0x2F, 0x2E, 0x2E, 0x2D,
    0x2D, 0x2C, 0x2C, 0x2B, 0x2B, 0x2A, 0x2A, 0x2A,
    0x29, 0x29, 0x28, 0x28, 0x27, 0x27, 0x26, 0x26,
    0x26, 0x25, 0x25, 0x24, 0x24, 0x24, 0x23, 0x23,
    0x22, 0x22, 0x22, 0x21, 0x21, 0x21, 0x20, 0x20,
    0x20, 0x1F, 0x1F, 0x1E, 0x1E, 0x1E, 0x1D, 0x1D,
    0x1D, 0x1C, 0x1C, 0x1C, 0x1B, 0x1B, 0x1B, 0x1A,
    0x1A, 0x1A, 0x19, 0x19, 0x19, 0x18, 0x18, 0x18,
    0x18, 0x17, 0x17, 0x17, 0x16, 0x16, 0x16, 0x15,
    0x15, 0x15, 0x15, 0x14, 0x14, 0x14, 0x13, 0x13,
    0x13, 0x13, 0x12, 0x12, 0x12, 0x12, 0x11, 0x11,
    0x11, 0x11, 0x10, 0x10, 0x10, 0x10, 0x0F, 0x0F,
    0x0F, 0x0F, 0x0E, 0x0E, 0x0E, 0x0E, 0x0D, 0x0D,
    0x0D, 0x0D, 0x0C, 0x0C, 0x0C, 0x0C, 0x0B, 0x0B,
    0x0B, 0x0B, 0x0A, 0x0A, 0x0A, 0x0A, 0x09, 0x09,
    0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07,
    0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* The pitch-modulation sensitivity's verbatim table (ROM
 * TABLE_PITCH_MOD_SENS), indexed by the patch's 3-bit parameter. */
const uint8_t slopsix_pitch_mod_sens[8] = {
    0x00, 0x0A, 0x14, 0x21, 0x37, 0x5C, 0x99, 0xFF
};
