/*
 * slopsix_output.c - The DX7S output stage (16-bit PCM54HP DAC).
 *
 * The DX7S output chain:
 *
 *   per-voice carrier sum  ->  raw bus sum (int32, no compander)
 *                          ->  16-bit DAC code step with saturation
 *                          ->  the DX7S analog tail filter
 *                          ->  optional pan split (SLOPSIX_EXTRAS only)
 *                          ->  the output volume (Q16)
 *                          ->  int16 conversion with rounding
 *
 * Unlike the DX7 Mk1, the DX7S has no compander anywhere in its path.
 * The service manual shows a PCM54HP 16-bit PCM DAC directly fed by the
 * OPS2's digital output, so the voices sum at the operator core's own
 * resolution and the 16-bit step, not a 12-bit compander word, is the
 * first quantization after the operators.  Keeping the compander here
 * would throw away the low bits the wider DAC exists to carry.
 *
 * The value that leaves the DAC step is a 16-bit code (range -32768..32767).
 * The filter that sits after the DAC models the board's analog
 * reconstruction filter (the DX7S's own stage, different from the Mk1's);
 * its output is continuous, so the integer grid is not preserved.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/* The DX7S per-voice bus stage: no compander.  The voice word is the
 * operator core's carrier sum at its own resolution, and that is what the
 * 16-bit DAC step must see; quantizing it to a 12-bit mantissa first
 * would discard the precision the wider DAC carries. */
void slopsix_dx7s_voice_bus(const int32_t *voice_out, int32_t *bus,
    unsigned count)
{
    unsigned s;

    for (s = 0; s < count; s++) {
        bus[s] += voice_out[s];
    }
}

/* The mixed bus's master DAC step for the DX7S: the bus value drops onto
 * the hardware's 16-bit code grid and saturates at the rails.
 *
 * The PCM54HP is a direct 16-bit DAC: there is no compander in the
 * master/output path and no 12-bit code grid, unlike the Mk1's master
 * stage.  The bus keeps the Mk1 bus's unit - the operator core's carrier
 * sum, whose single-voice full scale is 8 * 2047 = 16376 - so the 16-bit
 * grid sits at half a bus unit.  The 16-bit DAC spans the same analog
 * window as the Mk1's 12-bit one with sixteen times the codes (32768
 * against 2048), and the Mk1's grid sits at eight bus units per code
 * (SLOPSIX_DAC_GRID_SHIFT), so one 16-bit code is half a bus unit.
 *
 * The consequence is the reference level: a single full-scale voice
 * reaches the 16-bit rails exactly as it reaches the Mk1's 12-bit rails,
 * so the two personalities share one level and the DX7S spends the DAC's
 * extra codes on resolution rather than on headroom.  Sixteen voices sum
 * to 262016 bus units, well past the rails, so the step saturates there.
 *
 * Evidence grade: proportional estimate (hypothesis-grade, D7S-13).
 * The service manual gives the part and word width but not the digital
 * scaling or headroom; the rails and the grid's placement are the
 * engine's pinning. */
int32_t slopsix_dx7s_dac_step(int32_t bus)
{
    int32_t code;

    if (bus >= SLOPSIX_DX7S_DAC_BUS_MAX) {
        code = SLOPSIX_DX7S_DAC_CODE_MAX;
    } else if (bus < SLOPSIX_DX7S_DAC_BUS_MIN) {
        code = SLOPSIX_DX7S_DAC_CODE_MIN;
    } else {
        code = bus * 2;
    }

    return code;
}

/* The final conversion: the analog stage's output times a Q16 gain onto
 * int16.
 *
 * The value is the filter's output in Q12 of a 16-bit DAC code, so the
 * product is scaled by 2^28 (the gain's Q16 and the value's Q12) and
 * the gain falls out of the shift.  The value rounds to nearest,
 * half away from zero.
 *
 * The reference level follows the same convention as the Mk1: with no
 * headroom the output's full-scale window maps onto the int16 rails;
 * one bit of headroom halves it (6.02 dB).  The actual hardware level
 * is out of the digital model (the DAC's absolute span and the analog
 * stage are unknown, research OQ-24/OU-04).
 *
 * The analog stage can overshoot the 16-bit code window (the filter's
 * passband peaks at +0.8 dB in the Mk1 design; the DX7S filter is
 * similar), so the conversion saturates at the int16 rails: that
 * saturation is the chain's last one. */
int16_t slopsix_dx7s_output_scale(int32_t code_q12, int32_t gain_q16)
{
    int64_t product = (int64_t)code_q12 * (int64_t)gain_q16;
    int64_t magnitude = product < 0 ? -product : product;
    int64_t rounded = (magnitude +
        (1LL << (27 + SLOPSIX_OUTPUT_HEADROOM_BITS))) >>
        (28 + SLOPSIX_OUTPUT_HEADROOM_BITS);
    int64_t value = product < 0 ? -rounded : rounded;

    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }

    return (int16_t)value;
}

/* The once-per-sample composite for the DX7S: the 16-bit DAC step, the
 * analog tail's filter, the compile-time-selected pan and the master
 * conversion.
 *
 * The DX7S analog tail is its own stage (src/dx7s/slopsix_filter.c).  The
 * filter core takes the DAC code in Q12 internally, so a full-scale
 * 16-bit code enters directly - no downshift.  The output is Q12 of the
 * same 16-bit code, which is what slopsix_dx7s_output_scale expects (its
 * 28-bit shift folds out the 12 signal bits and the Q16 gain).
 *
 * Evidence grade: provisional prototype (hypothesis-grade, D7S-13).  The
 * DX7S output stage's exact response is not pinned from a schematic or a
 * capture yet, so the DX7S filter file carries the Mk1 candidate fit as a
 * placeholder.  The separation is deliberate: a later DX7S measurement
 * replaces that prototype table, not the shared core and not the DX7's
 * tail. */
void slopsix_dx7s_output_mix(void *vstate, int32_t bus,
                              int16_t *left, int16_t *right)
{
    slopsix_context_state *state = (slopsix_context_state *)vstate;
    int32_t code = slopsix_dx7s_dac_step(bus);
    int32_t value;
    int32_t left_code;
    int32_t right_code;
    int32_t gain = state->master_gain_q16;

    value = slopsix_filter_process(&state->filter, code);

#if defined(SLOPSIX_EXTRAS)
    /* The pan law is the shared one: the extras' pan is a host-side
     * convenience, not the personality's own hardware. */
    slopsix_pan_split(state->pan_q16, value, &left_code, &right_code);
#else
    left_code = value;
    right_code = value;
#endif

    *left = slopsix_dx7s_output_scale(left_code, gain);
    *right = slopsix_dx7s_output_scale(right_code, gain);
}
