/*
 * slopsix_output.c - The output stages: per-voice compander quantization,
 * the bus sum, the DAC step, pan and master volume.
 *
 * The per-voice compander quantizer is inline in slopsix_internal.h
 * (it runs for every voice on every sample).  This translation unit holds
 * the once-per-sample stages and the master-volume ladder:
 *
 *   mixed bus  ->  12-bit DAC code step with saturation
 *              ->  the analog tail's reconstruction filter (the DX7's
 *                 own stage, src/slopsix_filter.c)
 *              ->  optional pan split (SLOPSIX_EXTRAS only)
 *              ->  3-bit master volume (the DAC reference ladder)
 *              ->  int16 conversion with rounding
 *
 * The stages are separate functions with their own fixed widths, so a
 * later extension can replace the quantization or the conversion without
 * touching the mixer or the operators (plan decision D-15).  The pan is
 * selected at compile time; the DX7 path carries no runtime branch for it.
 *
 * The value that leaves this stage is in Q12 of a DAC code: the filter
 * that sits between the DAC step and the pan is an analog stage, so its
 * output is not on the DAC's integer grid.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/* * The master-volume ladder.
 *
 * The hardware's 3-bit DAC reference is a resistor ladder selected by an
 * analog multiplexer: 2.2K, 1.2K, 680, 330, 180, 100, 100, 100 ohms from
 * +5 V to ground, with one tap per level (the service-manual output
 * sheet, distilled on the dx7-output research page).  A tap's voltage is
 * 5 V times the resistance below it over the string's 4890 ohms, so the
 * taps run 0.102, 0.204, 0.307, 0.491, 0.828, 1.524, 2.751, 5.000 V.
 *
 * The public level 0..7 is the monotonic ladder order - level 0 the
 * bottom tap, level 7 the top - and each entry is the tap's gain as a
 * Q16 fraction of the top tap: round(65536 * R_below / 4890).  The
 * ladder's steps are deliberately not a pure exponential (about 5.2 dB
 * at the top, finer at the bottom), and volume 0 is about -33.8 dB, not
 * a mute: the ladder's own order, not an invented taper.
 *
 * The DAC's absolute span and the board's analog stage stay out of the
 * digital model together with the analog tail (research OQ-24, OU-04);
 * only the tap ratios are hardware facts here. */
static const int32_t slopsix_master_ladder_q16[8] = {
    1340,                /* 100 ohm below the tap - 0.102 V */
    2680,                /* 200 ohm - 0.204 V */
    4021,                /* 300 ohm - 0.307 V */
    6433,                /* 480 ohm - 0.491 V */
    10856,               /* 810 ohm - 0.828 V */
    19969,               /* 1490 ohm - 1.524 V */
    36052,               /* 2690 ohm - 2.751 V */
    SLOPSIX_Q16_ONE      /* 4890 ohm - 5.000 V, the top tap */
};

int32_t slopsix_master_gain_q16(unsigned level)
{
    if (level > 7u) {
        level = 7u;
    }
    return slopsix_master_ladder_q16[level];
}

/* The mixed bus's master DAC step (plan decision D-5): the bus value
 * drops onto the hardware's 12-bit code grid and saturates at the code
 * window's rails.  The grid sits at SLOPSIX_DAC_GRID_SHIFT of the bus's
 * unit because one voice's full compander scale is eight codes; see the
 * constant block for the model's grade. */
int32_t slopsix_dac_step(int32_t bus)
{
    int32_t code = slopsix_floor_div(bus, SLOPSIX_DAC_GRID_SHIFT);

    if (code > SLOPSIX_DAC_CODE_MAX) {
        code = SLOPSIX_DAC_CODE_MAX;
    } else if (code < SLOPSIX_DAC_CODE_MIN) {
        code = SLOPSIX_DAC_CODE_MIN;
    }

    return code;
}

/* The final conversion: the analog stage's output times a Q16 gain onto
 * int16.
 *
 * The value is the filter's output in Q12 of a DAC code, so int16 full
 * scale is 16 codes (2048 * 16 = 32768) at the code's own scale and the
 * gain falls out of the shift: the product is scaled by 2^24 (the gain's
 * Q16 and the value's Q12).  The value rounds to nearest, half away from
 * zero.
 *
 * The analog stage can overshoot the code window (the filter's passband
 * peaks at +0.8 dB and its high-Q section rings), so the conversion
 * saturates at the int16 rails: that saturation is the chain's last one.
 */
int16_t slopsix_output_scale(int32_t code_q12, int32_t gain_q16)
{
    int64_t product = (int64_t)code_q12 * (int64_t)gain_q16;
    int64_t magnitude = product < 0 ? -product : product;
    int64_t rounded = (magnitude + (1LL << 23)) >> 24;
    int64_t value = product < 0 ? -rounded : rounded;

    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }

    return (int16_t)value;
}

#if defined(SLOPSIX_EXTRAS)
/* The extras pan law.  The mono signal keeps unity gain in the half of
 * the travel it is panned towards and fades linearly across the other
 * half; at centre both amounts are the Q15 unity, so the split is
 * exactly the mono signal in both channels.  The amounts are Q15
 * (32768 == unity gain), so the split is one floor-division shift.
 *
 * The mono value is the filter's output in Q12 of a DAC code, so the
 * product is 64-bit. */
void slopsix_pan_split(int32_t pan_q16, int32_t mono, int32_t *left,
                       int32_t *right)
{
    int32_t left_amount;
    int32_t right_amount;

    /* The context keeps the pan inside 0..65536; the clamp is repeated
     * here so a caller-supplied position behaves like the setter's. */
    if (pan_q16 < 0) {
        pan_q16 = 0;
    } else if (pan_q16 > SLOPSIX_Q16_ONE) {
        pan_q16 = SLOPSIX_Q16_ONE;
    }

    if (pan_q16 <= SLOPSIX_Q16_ONE / 2) {
        left_amount = SLOPSIX_Q16_ONE / 2;
        right_amount = pan_q16;
    } else {
        left_amount = SLOPSIX_Q16_ONE - pan_q16;
        right_amount = SLOPSIX_Q16_ONE / 2;
    }

    *left = (int32_t)slopsix_floor_div64((int64_t)mono * left_amount, 15u);
    *right = (int32_t)slopsix_floor_div64((int64_t)mono * right_amount, 15u);
}
#endif

/* The once-per-sample composite: the DAC step, the analog tail's
 * filter, the compile-time-selected pan and the master conversion.
 * Without SLOPSIX_EXTRAS the mono code lands in both channels unchanged -
 * the faithful centre output. */
void slopsix_output_mix(slopsix_context_state *state, int32_t bus,
                        int16_t *left, int16_t *right)
{
    int32_t code = slopsix_dac_step(bus);
    int32_t value = slopsix_filter_process(&state->filter, code);
    int32_t left_code;
    int32_t right_code;
    int32_t gain;

#if defined(SLOPSIX_EXTRAS)
    slopsix_pan_split(state->pan_q16, value, &left_code, &right_code);
#else
    left_code = value;
    right_code = value;
#endif

    gain = slopsix_master_gain_q16(state->master_volume);
    *left = slopsix_output_scale(left_code, gain);
    *right = slopsix_output_scale(right_code, gain);
}
