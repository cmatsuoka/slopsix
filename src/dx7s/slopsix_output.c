/*
 * slopsix_output.c - The DX7S output stage (the PCM54HP driven by the
 * OPS2's 15-bit DA2-DA16 word).
 *
 * The DX7S output chain:
 *
 *   per-voice carrier sums ->  wide digital accumulator (int32, the
 *                             operator words summed linearly, no
 *                             compander, no per-voice clamp)
 *                          ->  the transmitted DAC word: the
 *                             accumulator's sum divided by sixteen, on
 *                             the 15-bit DA2-DA16 rails (the DAC's top
 *                             Bit 1 runs grounded)
 *                          ->  the DX7S analog tail filter
 *                          ->  optional pan split (SLOPSIX_EXTRAS only)
 *                          ->  the output volume (Q16)
 *                          ->  int16 delivery with the analog restore
 *                             gain, through the analog ceiling's
 *                             compression
 *
 * Unlike the DX7 Mk1, the DX7S has no compander anywhere in its path.
 * The service manual's IC pin list shows the OPS2 emitting the DAC
 * code on fifteen lines (DA2-DA16, "digital code for analog convert")
 * with the SH1/SH2 hold timing, and the PCM54HP's top Bit 1 runs
 * grounded on the Overall Circuit Diagram: the transmitted word is
 * 15-bit **linear**, and the pin group carries no mantissa/shift codes
 * - the compander's interface is absent at the chip boundary (research
 * dx7s 19.4.2).
 *
 * The transmitted word holds the whole polyphonic sum.  Sixteen voices
 * at full scale sum to 16 * 16376 = 262016 bus units; dividing by
 * sixteen - the "discard the excess least significant bits" step -
 * puts that sum's peak on 16376, just inside the 15-bit rails
 * (+16383/-16384).  The DAC word therefore never saturates for any
 * polyphony: a unison stack rides the word the way sixteen 12-bit
 * voice words fill a 16-bit accumulator, and the machine's analog
 * stage (the scaled hold legs and the volume VCA) restores the line
 * level outside the model (research OQ-24/OU-04): the delivery applies
 * that restore gain (the DAC code times eight), landing the delivered
 * difference at the real-device figure (about -6 dB per full-scale
 * voice), and the delivered int16 rail stands where the model places
 * the board's output swing.  Content past the swing - hot unison
 * stacks above three stacked windows - compresses through the analog
 * ceiling's curve; typical content below it stays exact.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/* The DX7S per-voice bus stage: the wide accumulator.  There is no
 * compander: the voice word is the operator core's carrier sum at its
 * own resolution, summed linearly.  Any per-voice word width the OPS2
 * owns stays inside the chip (the COM law of algorithm page §4.4 keeps
 * multi-carrier voices at about one window); the accumulator's own
 * width - the reviewer-model's "16-bit or higher" - is the model's
 * int32, wide enough that sixteen 14-bit signals never approach its
 * rails.  There is no per-voice output stage at all in this reading:
 * the chip's only transmitted word is the mixed DAC code. */
void slopsix_dx7s_voice_bus(const int32_t *voice_out, int32_t *bus,
    unsigned count)
{
    unsigned s;

    for (s = 0; s < count; s++) {
        bus[s] += voice_out[s];
    }
}

/* The mixed bus's DAC word for the DX7S: the accumulator's sum divided
 * by sixteen, on the 15-bit DA2-DA16 rails.
 *
 * The bus keeps the Mk1 bus's unit - the operator core's carrier sum,
 * whose single-voice full scale is 8 * 2047 = 16376 - so the sum of
 * sixteen full-scale voices is 262016 bus units.  Dividing by sixteen
 * (dropping the four least significant bus bits) puts that peak on
 * 16376, inside the 15-bit transmitted word's +16383/-16384 rails: a
 * unison stack fills the word the way sixteen 12-bit voice words fill
 * a 16-bit accumulator before its own LSB is dropped (research dx7s
 * 19.4).  The DAC word never saturates for any polyphony the engine
 * can sum; the rails clamp only a hypothetical bus beyond sixteen
 * windows, which the voice stage's COM law keeps off.
 *
 * Evidence grade: hypothesis-grade (D7S-13, 2026-11-21).  The pin
 * evidence (fifteen DA lines, the grounded Bit 1, the SH1/SH2 hold
 * timing) pins the transmitted word's width and linearity; the
 * divide-by-sixteen is what sixteen voice windows require to land
 * inside it with zero overflow, and the analog side's restore gain
 * (the scaled legs and the VCA) stays out of the model.  The earlier
 * readings of this stage: per-slot 16-bit words (2026-11-19), a
 * premixed master step at the Mk1's grid, per-slot 15-bit words with
 * analog summing, and a parity level with analog saturation at the
 * delivery rails - each was refuted by the audible reports or by the
 * pin evidence; the wide-accumulator premix is the reading the
 * maintainer pinned from both. */
int32_t slopsix_dx7s_dac_step(int32_t bus)
{
    int32_t code = slopsix_floor_div(bus, SLOPSIX_DX7S_DA_SHIFT);

    if (code > SLOPSIX_DX7S_DA_MAX) {
        code = SLOPSIX_DX7S_DA_MAX;
    } else if (code < SLOPSIX_DX7S_DA_MIN) {
        code = SLOPSIX_DX7S_DA_MIN;
    }

    return code;
}

/* The final conversion: the analog stage's output times a Q16 gain onto
 * int16, through the analog ceiling's compression.
 *
 * The value is the filter's output in Q12 of the DAC code, so the
 * product is a 64-bit multiply; the shift is
 * SLOPSIX_DX7S_OUTPUT_SCALE_SHIFT - the DAC code times eight - the
 * machine's analog restore gain relative to the transmitted word: the
 * delivered difference against the Mk1's hot delivered reference lands
 * at the measurable real-device figure (about -6 dB per full-scale
 * voice, one voice at 8184).  The value rounds to nearest, half away
 * from zero.
 *
 * The analog ceiling follows: the board's output stages (the
 * reconstruction filter, then the volume VCA and the output amplifier)
 * run their swing against the restored line level, and the delivered
 * int16 rail is where this model places that swing.  Above
 * SLOPSIX_DX7S_OUTPUT_SATURATION_KNEE the value compresses toward the
 * rail with a rational soft clip - continuous with unit gain at the
 * knee, asymptotic below the rail - so a hot stack's top is limited
 * the way an output amp approaches its swing, not flat-clipped by a
 * digital boundary.  Content up to about three stacked Mk1 windows
 * stays exact; hotter plateaus lose their last two to three decibels
 * to the curve - the analogue of the real machine's output stage
 * running into its swing at the same calibration.
 *
 * Evidence grade: hypothesis-grade (D7S-13).  The delivered gain (-6
 * dB) is the maintainer's pin from the real-device measurement; the
 * knee's placement and the curve stand in for the output stage's
 * saturation, which no capture pins yet.  The real machine's analog
 * ceiling sits above its line level with more room than a 16-bit
 * deliverable at this calibration carries - the curve is what the
 * int16 delivery keeps of that headroom. */
int16_t slopsix_dx7s_output_scale(int32_t code_q12, int32_t gain_q16)
{
    int64_t product = (int64_t)code_q12 * (int64_t)gain_q16;
    int64_t magnitude = product < 0 ? -product : product;
    int64_t rounded = (magnitude +
        (1LL << (SLOPSIX_DX7S_OUTPUT_SCALE_SHIFT - 1))) >>
        SLOPSIX_DX7S_OUTPUT_SCALE_SHIFT;
    int64_t saturated = rounded;
    int64_t value;

    if (saturated > SLOPSIX_DX7S_OUTPUT_SATURATION_KNEE) {
        int64_t excess = saturated - SLOPSIX_DX7S_OUTPUT_SATURATION_KNEE;
        int64_t span = SLOPSIX_DX7S_OUTPUT_RAIL -
                       SLOPSIX_DX7S_OUTPUT_SATURATION_KNEE;

        /* Both operands are positive on the magnitude path, so the
         * division floors by itself. */
        saturated = SLOPSIX_DX7S_OUTPUT_RAIL -
            span * span / (2 * excess + span);
    }

    value = product < 0 ? -saturated : saturated;

    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }

    return (int16_t)value;
}

/* The once-per-sample composite for the DX7S: the DAC word, the analog
 * tail's filter, the compile-time-selected pan and the master
 * conversion.
 *
 * The DAC word is the model's transmitted signal ("digital code for
 * analog convert"); the DX7S analog tail (src/dx7s/slopsix_filter.c)
 * is its own stage through the shared filter core, which scales the
 * code to Q12 internally - the word's values (up to the 15-bit rails)
 * fit the Q12 domain with room for the low-pass's overshoot.  The
 * output is Q12 of the same unit, which the conversion's restore gain
 * and analog ceiling fold onto int16.
 *
 * Evidence grade: provisional prototype (hypothesis-grade, D7S-13).
 * The DX7S output stage's exact response is not pinned from a schematic
 * or a capture yet, so the DX7S filter file carries the Mk1 candidate
 * fit as a placeholder.  A later DX7S measurement replaces that
 * prototype table, not the shared core and not the DX7's tail. */
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
