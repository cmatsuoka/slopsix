/*
 * slopsix_operator.c - The operator core: the phase accumulator, the
 * log-sine lookup, the signal exponent and the feedback pair.
 *
 * The pipeline follows the hardware's (research O-02, O-04):
 *
 *   sine phase (12 bits)  ->  log-sine (Q10, negated log2 of |sin|)
 *   log amplitude (Q8)    ->  14-bit log word (Q10)
 *   log word              ->  14-bit linear sample (the signal exp)
 *
 * The modulation input is a phase addition: the modulating operator's
 * linear output is added to the modulated operator's phase before the
 * sine lookup (the patent's sin{K.wt + f(w.mt)} form).
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/* The generated tables (shared const data, plan decision D-4). */
extern const int16_t slopsix_logsin_quarter[1024];
extern const uint16_t slopsix_exp_q11[1024];

/*
 * The log-sine lookup.
 *
 * The sine circuit consumes a 12-bit phase: the top bit selects the wave
 * half and inverts the output, the second bit selects the quarter and
 * works by complementing the 10-bit index, and the low 10 bits are the
 * quarter-wave ROM index.  The returned value is the negated log2 of the
 * sine's magnitude in Q10 units (1024 per octave); the caller applies the
 * half's sign.
 */
int32_t slopsix_logsin(uint32_t sine_phase)
{
    uint32_t index = sine_phase & SLOPSIX_SINE_PHASE_MASK;
    uint32_t quarter = index & 0x3FFu;

    if (index & 0x400u) {
        /* The second quarter of the peak: the index is complemented. */
        quarter = 1023u - quarter;
    }

    return (int32_t)slopsix_logsin_quarter[quarter];
}

/*
 * The signal exponent.
 *
 * The 14-bit log word (Q10: four integer octaves and a ten-bit fraction)
 * is converted to the linear sample with the hardware's recipe:
 *
 *   value = round(2^frac * 2048) << int >> 13
 *
 * The ROM's fraction table is the generated exp table; the integer part
 * is the chip's four-bit field, so a word above the field's range
 * saturates at the top and a word below zero (below the exponent range)
 * is silence.
 */
int32_t slopsix_signal_exp(int32_t log_q10)
{
    int32_t int_part;
    int32_t frac;
    uint32_t value;

    if (log_q10 < 0) {
        /* Below the exponent range: silence. */
        return 0;
    }

    int_part = log_q10 >> 10;
    frac = log_q10 & 1023;

    if (int_part > 15) {
        /* The integer field is four bits: the value saturates. */
        int_part = 15;
        frac = 1023;
    }

    value = ((uint32_t)slopsix_exp_q11[frac] << (uint32_t)int_part) >> 13;

    return (int32_t)value;
}

/*
 * The feedback path.
 *
 * The hardware averages the last two samples of the feedback path before
 * feeding them back (the die's two feedback registers and the averaging
 * adder), and the result is divided by a power of two (the 0..6-bit
 * feedback shifter).  The level-to-shift mapping is provisional (research
 * O-06); the path's own value is the sum of the outputs that the
 * algorithm's control bits add to it.
 */
int32_t slopsix_feedback_scaled(int32_t fb_prev, int32_t fb_prev2,
                                uint8_t feedback_level)
{
    int32_t value;
    int32_t shift;

    /* The feedback field is the OPS's three-bit field, 0..7; a value
     * outside it is out of contract and is clipped here, once, so the
     * shifter's exponent stays in range on the render path (research
     * O-06). */
    if (feedback_level > 7u) {
        feedback_level = 7u;
    }

    if (feedback_level == 0u) {
        return 0;
    }

    /* The average of the last two samples of the path. */
    value = (fb_prev + fb_prev2) >> 1;

    /* The feedback shifter's division. */
    shift = (int32_t)SLOPSIX_FB_SHIFT(feedback_level);
    value >>= shift;

    /* The path's value is in the operator's output units, which are the
     * 14-bit signal exp's range; the deviation scale assumes that
     * range. */
    if (value > 16383) {
        value = 16383;
    } else if (value < -16383) {
        value = -16383;
    }

    return value;
}

/*
 * Render one operator.
 *
 * The phase advances by the operator's increment; the modulation input
 * (and the operator's own feedback) are added to the phase before the
 * sine lookup.  The output is the 14-bit linear sample.
 */
void slopsix_op_render(slopsix_operator *op, const slopsix_op_params *params,
                       int32_t mod_in, int32_t *out)
{
    uint32_t acc;
    uint32_t phase;
    uint32_t sine_phase;
    int32_t log_value;
    int32_t sample;

    /* The modulation input - the sum of the operator's input buses, or
     * the feedback path's value - is added to the phase for the lookup
     * only: it is not part of the accumulator's state.  The input is in
     * the 14-bit signal exp's range, which is the range the deviation
     * scale is defined over. */
    if (mod_in > 16383) {
        mod_in = 16383;
    } else if (mod_in < -16383) {
        mod_in = -16383;
    }

    acc = op->phase;
    phase = acc + (uint32_t)(mod_in * (1 << SLOPSIX_MOD_DEVIATION_SHIFT));

    sine_phase = (phase >> SLOPSIX_SINE_PHASE_SHIFT) & SLOPSIX_SINE_PHASE_MASK;

    /* The log word: the operator's log amplitude (Q8, shifted into the
     * log word's Q10 scale) minus the log-sine's negated log2 value. */
    log_value = (params->log_amp_q8 << SLOPSIX_AMP_Q8_SHIFT) -
                slopsix_logsin(sine_phase);

    sample = slopsix_signal_exp(log_value);

    if (sine_phase & 0x800u) {
        /* The negative half of the wave inverts the output. */
        sample = -sample;
    }

    op->phase = (acc + op->increment) & SLOPSIX_PHASE_MASK;

    *out = sample;
}
