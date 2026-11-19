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
 * speed-first (-O3); the rest of the library is compiled size-first.
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
 * P-03, hypothesis-grade); it receives neither the master tune nor the
 * voice's pitch.
 *
 * The voice's pitch contributions - the master tune, the pitch bend and
 * the pitch EG - reach the operator in ratio mode only: the EGS adds
 * them to the voice frequency, which a fixed-frequency operator does not
 * read (research P-03, a bit-synchronous EGS model).  The bend and the
 * pitch EG arrive as one log-frequency offset; both denote a change in
 * the same voice pitch.
 *
 * The word's width.
 *
 * The engine's word is the firmware's own 16-bit, 4096-per-octave word
 * (plan decision D-12), but the engine composes the whole operator
 * frequency in one accumulator and quantizes the result once, at the
 * EGS's 14-bit frequency field (D-16).  The composed word is therefore
 * a model-domain quantity, not a hardware register: the coarse and fine
 * ratios are msfa-derived offsets (P-02, model-grade) that the hardware
 * keeps in the EGS's own operator-frequency register, so the hardware's
 * 16-bit voice register does not bound them either.
 *
 * What the hardware does bound is the *voice* word, and the engine
 * models that: the firmware clamps the voice-frequency word at its floor
 * before the master tune and the EGS's operator frequency are added
 * (research P-01's composition chain), so a voice that pitches far
 * below its note stops at the floor while its operator keeps its own
 * ratio.  The clamp below is that floor, in the engine's pre-bias word
 * units.  The master tune is added after the clamp, as on the hardware.
 */
/*
 * The voice's frequency word, shared by every ratio-mode operator of a
 * voice and therefore composed once per frequency rewrite rather than
 * once per operator.
 *
 * It is the note's word plus the voice's pitch offset (the
 * pitch-modification register's word - the bend and the LFO's per-tick
 * wave term - and the pitch EG level).  The firmware clamps this word at
 * the floor before the master tune and the operator's own frequency are
 * added; the engine's offsets arrive as one, so the clamp covers them all
 * (the register's own hardware path is the EGS's pitch-modification
 * register, which the firmware does not clamp - a model-level deviation,
 * noted above).  The master tune is added after the clamp, as on the
 * hardware.
 */
int32_t slopsix_voice_word_from_base(int32_t base_word,
                                     int32_t master_tune_q16,
                                     int32_t pitch_offset)
{
    /* The master tune, in Q16 cents, becomes the word's units: 4096
     * units per octave is 1200 cents, so one cent is 4096/1200 units. */
    int32_t tune = master_tune_q16 / 19200;
    int32_t voice = base_word;

    voice += pitch_offset;
    if (voice < SLOPSIX_FREQ_VOICE_FLOOR) {
        voice = SLOPSIX_FREQ_VOICE_FLOOR;
    }

    return voice + tune;
}

int32_t slopsix_voice_word(uint8_t note, int32_t master_tune_q16,
                           int32_t pitch_offset)
{
    return slopsix_voice_word_from_base(
        (int32_t)slopsix_note_word[note & 127u],
        master_tune_q16, pitch_offset);
}

/*
 * One operator's frequency word from its voice's precomposed word.
 *
 * Ratio mode adds the operator's coarse and fine ratios, which the EGS
 * adds after the voice word; fixed-frequency mode ignores the voice and
 * uses the operator's own frequency.  Both modes add the same detune law,
 * so the fixed mode's detune is antisymmetric around the neutral value
 * too.
 */
int32_t slopsix_op_freq_from_voice(const slopsix_op_params *params,
                                   int32_t voice_word)
{
    int32_t word;
    uint8_t detune;

    /* The hardware's detune is a four-bit signed field, 0..14; a value
     * outside it is out of contract and is clipped here, once, so the
     * render and the note-on path never see one (research P-01). */
    detune = params->detune;
    if (detune > 14u) {
        detune = 14u;
    }

    if (params->mode == 0u) {
        /* The operator's own frequency (the coarse and fine ratios) is
         * added by the EGS after the voice word.  The firmware's fine
         * lookup has 101 words (0..100): a serialised fine value of 100
         * reads the extra word 0x1000, so coarse 0/fine 100 is the 1.0
         * ratio, not the 0.5 ratio.  Values above 100 read past the
         * table on the hardware; the engine clamps them to the last
         * defined word so the render stays deterministic. */
        unsigned fine = params->fine;

        if (fine > 100u) {
            fine = 100u;
        }
        word = voice_word;
        word += slopsix_coarse_offset[params->coarse & 31u];
        word += (int32_t)slopsix_fine_offset[fine];
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
        /* The fixed fine term is the raw 0..127 field multiplied by
         * 136 on the firmware, not the ratio table's index: a serialised
         * 100 adds a full decade.  Do not wrap it modulo 100. */
        int32_t index = (int32_t)(params->coarse & 3u) * 100 +
                        (int32_t)params->fine;

        word = ((4458616 * index) >> 15) -
               (SLOPSIX_LOG2_440_Q10 * 4) + SLOPSIX_WORD_A4;
    }

    word += slopsix_detune_offset(detune);

    /* The voice pitch - the note, master tune, pitch bend and pitch EG -
     * is already in the word in ratio mode; a fixed-frequency operator
     * uses its own frequency register plus detune (research P-03, a
     * bit-synchronous EGS model). */

    return word;
}

/*
 * The per-operator composition for callers that hold a note rather than a
 * voice word: the voice word is composed first and then handed to the
 * operator law.  The render path composes the voice word once per voice
 * and calls slopsix_op_freq_from_voice() directly.
 */
int32_t slopsix_op_freq_word(uint8_t note, const slopsix_op_params *params,
                             int32_t master_tune_q16, int32_t pitch_offset)
{
    return slopsix_op_freq_from_voice(params,
        slopsix_voice_word(note, master_tune_q16, pitch_offset));
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

    /* The hardware's composition: the voice's 16-bit word minus the
     * frequency bias, clamped at zero, then handed to the EGS's
     * 1024-per-octave field (research P-01).  The master tune and the
     * pitch offsets are already in the word, so only the bias
     * subtraction, the field truncation and the clamp remain here. */
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

/*
 * Portamento (research P-06).
 *
 * The firmware keeps three per-voice frequency buffers in its own
 * 4096-per-octave word: the note target, the portamento word and the
 * glissando word.  PORTA_PROCESS walks each voice once every second OCF
 * tick and moves the portamento word toward the target by
 *
 *   step = rate * ((high byte of |current - target| >> 2) + 1)
 *
 * i.e. a step proportional to the distance's top bits, not a constant
 * rate.  The high byte is the word's bits 15..8, so the bracket is the
 * distance's bits 10..15 plus one.  A step past the target clamps at
 * the target.  The rate is the pitch-EG rate table's entry indexed by
 * `99 - time`; time 0 gives 0xFF, the instantaneous setting.
 *
 * Glissando replaces the audible word with a semitone stair.  The step
 * is 0x154 (340) in the same word, the note table's three-semitone
 * cycle being 0x154, 0x154, 0x158; after adding or subtracting it the
 * firmware snaps the low byte back onto the note table's curve,
 * `84 * (high & 3)`, with the carry adjustment that keeps the cycle
 * (when the new high byte is 3 modulo 4, the high byte is advanced and
 * the low byte becomes zero).  The stair steps only when it is at
 * least one step away from the portamento word; if it has stepped, the
 * new stair word is what the voice hears, otherwise it keeps the
 * current one.
 */

static int32_t slopsix_gliss_quantize(int32_t word)
{
    uint32_t high = (uint32_t)word >> 8;

    /* The ROM's carry fix-up: a reconstruction landing on the cycle's
     * last entry (high & 3 == 3) advances to the next high byte with a
     * zero low byte, so the stair stays on note-table words. */
    if (((high + 1u) & 3u) == 0u) {
        high++;
    }

    return (int32_t)((high << 8) | (uint32_t)(84u * (high & 3u)));
}

int32_t slopsix_porta_step_word(int32_t *current, int32_t target,
                                int32_t *gliss, uint8_t rate,
                                bool glissando)
{
    int32_t diff = *current - target;

    if (diff > 0) {
        int32_t step = (int32_t)rate * ((diff >> 10) + 1);
        int32_t next = *current - step;

        *current = (next < target) ? target : next;
    } else if (diff < 0) {
        int32_t distance = target - *current;
        int32_t step = (int32_t)rate * ((distance >> 10) + 1);
        int32_t next = *current + step;

        *current = (next > target) ? target : next;
    }

    if (!glissando) {
        return *current;
    }

    diff = *gliss - *current;
    if (diff >= 0x154) {
        *gliss = slopsix_gliss_quantize(*gliss - 0x154);
    } else if (diff <= -0x154) {
        *gliss = slopsix_gliss_quantize(*gliss + 0x154);
    }

    return *gliss;
}
