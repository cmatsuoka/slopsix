/*
 * slopsix_pitch.c - The frequency path: the note's log-frequency word,
 * the per-operator frequency composition and the phase increment.
 *
 * Unit domains (plan decision D-12):
 *
 *   - The engine's word is the firmware's own 16-bit word with 4096
 *     units per octave.  The note's word is the firmware's closed form
 *     (research P-01): note 1 is the word's zero and the
 *     word counts 4096 units per octave.
 *   - The EGS's frequency datum is the same quantity at 1024 units per
 *     octave: the word shifted right by two, plus the calibration
 *     offset below.  The EGS's datum is what the chip's frequency
 *     exponent consumes, as a four-bit octave field and a ten-bit
 *     fraction.
 *   - The frequency exponent then produces the 22-bit phase increment
 *     with the hardware's recipe (research P-01).
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/*
 * The EGS's calibration offset.
 *
 * The EGS's datum for a note is (word - bias) >> 2 + offset.  The
 * offset is the one additive octave the EGS's internal datapath adds to
 * the CPU's word (research P-01's remainder); the engine anchors MIDI
 * note 69 to 440 Hz and carries the offset as a compile-time constant
 * until the official documentation or a measurement pins it exactly.
 *
 * With the offset below, note 69 (word 23208) yields the datum 10443,
 * whose exponent gives an increment of 75200 phase units per sample at
 * 49,096 Hz: 440.12 Hz.  That is the closest the hardware's frequency
 * exponent can come to 440 Hz: the increment's step is 32 phase units,
 * about 0.19 Hz at this pitch, so the engine's A4 sits half a cent
 * sharp.  The quantization is the hardware's own, not the engine's.
 */
#define SLOPSIX_EGS_OCTAVE_OFFSET \
    (SLOPSIX_EGS_DATUM_A4 - ((SLOPSIX_WORD_A4 - SLOPSIX_FREQ_BIAS) >> 2))

/*
 * The detune term.
 *
 * The EGS applies the patch's detune as a signed 4-bit value added
 * directly to its 14-bit frequency word (a bit-synchronous EGS model,
 * research P-04).  One EGS unit is 1/1024 octave, about 1.17 cents, and
 * the engine's word is four times finer, so one patch step is four word
 * units.  The patch's neutral detune is 7 (0..14 -> -7..+7), and the
 * offset is pitch-independent.
 */
static int32_t slopsix_detune_offset(uint8_t detune)
{
    return ((int32_t)detune - 7) * 4;
}

/*
 * The per-operator frequency word.
 *
 * Ratio mode (mode 0) composes the note's word with the coarse ratio,
 * the fine ratio and the detune term.  Fixed-frequency mode (mode 1)
 * ignores the note and uses the operator's fixed frequency (research
 * P-03, hypothesis-grade); it still receives the master tune.
 *
 * The voice's pitch contributions - the master tune and the pitch bend -
 * reach the operator in ratio mode only: the EGS adds them to the voice
 * frequency, which a fixed-frequency operator does not read (research
 * P-03, a bit-synchronous EGS model).
 */
int32_t slopsix_op_freq_word(uint8_t note, const slopsix_op_params *params,
                             int32_t master_tune_q16, int32_t bend_offset)
{
    int32_t word;
    int32_t tune;
    uint8_t detune;

    /* The master tune, in Q16 cents, becomes the word's units: 4096
     * units per octave is 1200 cents, so one cent is 4096/1200 units. */
    tune = master_tune_q16 / 19200;

    /* The hardware's detune is a four-bit signed field, 0..14; a value
     * outside it is out of contract and is clipped here, once, so the
     * render and the note-on path never see one (research P-01). */
    detune = params->detune;
    if (detune > 14u) {
        detune = 14u;
    }

    if (params->mode == 0u) {
        word = (int32_t)slopsix_note_word[note & 127u];
        word += slopsix_coarse_offset[params->coarse & 31u];
        word += (int32_t)slopsix_fine_offset[params->fine % 100u];
    } else {
        /* The fixed frequency.  The index is a decade-scaled log2 of
         * the frequency: frequency = 10^(index / 100), so one coarse
         * step (the field's two bits) is one decade and the fine field
         * splits the decade on that same log scale (research P-03,
         * hypothesis-grade).
         *
         * The model's formula, (4458616 * index) >> 3, is the model's
         * 2^24-per-octave log2(f) above 1 Hz: 4458616 / 8 = 557,327 =
         * 2^24 * log2(10) / 100, exactly.  Shifted down to the
         * engine's 4096-per-octave word it is 136.06 units per index
         * step.  The model's word counts the octaves above 1 Hz while
         * the note word counts them above note 1; the two constants
         * below perform that re-anchoring, A4's word (23,208) and
         * 4096 * log2(440) = 35,972. */
        int32_t index = (int32_t)(params->coarse & 3u) * 100 +
                        (int32_t)(params->fine % 100u);

        word = ((4458616 * index) >> 15) -
               (SLOPSIX_LOG2_440_Q10 * 4) + SLOPSIX_WORD_A4;
    }

    /* Both modes share the one detune law, so the fixed mode's detune is
     * antisymmetric around the neutral value too. */
    word += slopsix_detune_offset(detune);

    /* The EGS adds the voice pitch - the note, master tune, pitch bend
     * and pitch EG - only in ratio mode.  A fixed-frequency operator
     * uses its own frequency register plus detune (research P-03, a
     * bit-synchronous EGS model). */
    if (params->mode == 0u) {
        word += tune;
        word += bend_offset;
    }

    return word;
}

/*
 * The phase increment.
 *
 * The engine's word is truncated to the EGS's fourteen-bit frequency
 * field - the chip's 1024-per-octave grid - before the increment is
 * derived, so the engine's pitches are the ones the hardware can
 * actually produce (plan decision D-16).  The frequency exponent then
 * applies the hardware's recipe:
 *
 *   increment = round(2^frac * 2048) << int >> 5
 *
 * The recipe is the operator core's own exponential (slopsix_exp_value),
 * with the frequency path's own shift.
 *
 * The increment counts phase units per sample at the engine's own rate
 * of 49,096 Hz, so it is rescaled for a different host rate; the
 * frequency itself is rate-independent.
 */
uint32_t slopsix_increment_from_word(int32_t word, uint32_t sample_rate)
{
    int32_t datum;
    uint32_t increment;

    /* The firmware's composition: the note's word plus the pitch EG's
     * level, minus the pitch EG's neutral constant (the bias), clamped
     * at zero, with the master tune added afterwards (research P-01).  The engine adds the master tune into the word itself,
     * so only the bias subtraction and the clamp remain here. */
    datum = slopsix_floor_div(word - SLOPSIX_FREQ_BIAS, 2) +
            SLOPSIX_EGS_OCTAVE_OFFSET;

    if (datum < 0) {
        datum = 0;
    }
    if (datum > 16383) {
        datum = 16383;
    }

    increment = slopsix_exp_value(datum, 5u);

    /* Rescale from the engine's 49,096 Hz to the host's rate.
     *
     * floor(a * b / c) is kept inside 32-bit arithmetic with the
     * identity  a * b / c = q1 * q2 * c + q1 * r2 + q2 * r1 +
     * r1 * r2 / c,  where q and r are quotient and remainder of a and
     * b by c; with c = 49,096 the remainder product stays under 2^32.
     * The rate is capped at eight times the anchor rate, where the
     * rescaled increment saturates: every partial product then fits in
     * 32 bits for any host rate, and the increment aliases the
     * accumulator above that pitch in the same way the 23-bit phase
     * addition always does. */
    if (sample_rate != SLOPSIX_DEFAULT_SAMPLE_RATE) {
        uint32_t anchor = SLOPSIX_DEFAULT_SAMPLE_RATE;
        uint32_t rate = sample_rate;
        uint32_t r1 = increment % anchor;
        uint32_t r2;
        uint32_t q1 = increment / anchor;
        uint32_t q2;

        if (rate > anchor * 8u) {
            rate = anchor * 8u;
        }
        q2 = rate / anchor;
        r2 = rate % anchor;

        increment = q1 * q2 * anchor + q1 * r2 + q2 * r1 +
                    r1 * r2 / anchor;
    }

    return increment;
}
