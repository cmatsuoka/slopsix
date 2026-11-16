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

/* The generated tables (shared const data, plan decision D-4). */
extern const uint16_t slopsix_note_word[128];
extern const uint16_t slopsix_exp_q11[1024];

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

/* Floor division by a power of two, defined for negative values (the word
 * minus the bias can be negative for the lowest notes). */
static int32_t slopsix_floor_div(int32_t value, unsigned shift)
{
    if (value >= 0) {
        return value >> shift;
    }
    return -(((-value) + (int32_t)((1u << shift) - 1u)) >> shift);
}

/*
 * The detune term.
 *
 * The working model (research P-04, hypothesis-grade): the detune offset
 * is proportional to the note's absolute log frequency and decays with
 * the pitch,
 *
 *   delta = 0.0209 * exp(-0.396 * log2(f)) / 7 * logfreq * (detune - 7)
 *
 * with the model's logfreq in its own 2^24-per-octave word.  The engine
 * works in 4096-per-octave units, so the model's offset is divided by
 * 4096.
 *
 * The exponential is evaluated with the hardware's own exp table: the
 * model's exp(-0.396 * x) is 2^(-0.396 * log2(e) * x), and
 * 0.396 * log2(e) is 0.5713, kept as 585/1024.
 */
static int32_t slopsix_detune_offset(int32_t word, uint8_t detune)
{
    int32_t oct_q10;
    int32_t exp_q10;
    int32_t int_part;
    int32_t frac;
    uint32_t exp_q11;
    int32_t t;
    int32_t denominator;
    int32_t delta;

    if (detune == 7u) {
        /* The hardware's neutral detune (the model's centre). */
        return 0;
    }

    /* The absolute pitch in Q10: log2(f) = log2(440) + the word's offset
     * from A4. */
    oct_q10 = SLOPSIX_LOG2_440_Q10 +
              slopsix_floor_div(word - SLOPSIX_WORD_A4, 2);

    /* The exponent of the exponential: -0.5713 * log2(f), in Q10. */
    exp_q10 = -((SLOPSIX_DETUNE_EXP_COEF * oct_q10) >> 10);

    int_part = slopsix_floor_div(exp_q10, 10);
    frac = exp_q10 - (int_part << 10);

    /* The ROM's fraction value; the integer part of the exponent enters
     * through the denominator below. */
    exp_q11 = slopsix_exp_q11[frac];

    /* The model's term: exp * logfreq * (detune - 7), converted to the
     * engine's word.  The factor 172/2^22 folds the model's 0.0209 and
     * the 4096-units-per-octave conversion. */
    t = (int32_t)exp_q11 * oct_q10;
    t *= (int32_t)detune - 7;

    /* delta = 172 * t / (7 * 2^(22 - int_part)), computed in two steps to
     * stay inside 32 bits. */
    denominator = 7 << (14 - int_part);
    delta = ((t >> 8) * (SLOPSIX_DETUNE_COEF_Q11 * 4) +
             (denominator >> 1)) / denominator;

    return delta;
}

/*
 * The per-operator frequency word.
 *
 * Ratio mode (mode 0) composes the note's word with the coarse ratio,
 * the fine ratio and the detune term.  Fixed-frequency mode (mode 1)
 * ignores the note and uses the operator's fixed frequency (research
 * P-03, hypothesis-grade); it still receives the master tune.
 */
int32_t slopsix_op_freq_word(uint8_t note, const slopsix_op_params *params,
                             int32_t master_tune_q16)
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
        word += slopsix_detune_offset(word, detune);
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

        /* Both modes share the one detune law, so the fixed mode's
         * detune is antisymmetric around the neutral value too. */
        word += slopsix_detune_offset(word, detune);
    }

    return word + tune;
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
 * The increment counts phase units per sample at the engine's own rate
 * of 49,096 Hz, so it is rescaled for a different host rate; the
 * frequency itself is rate-independent.
 */
uint32_t slopsix_increment_from_word(int32_t word, uint32_t sample_rate)
{
    int32_t datum;
    int32_t int_part;
    int32_t frac;
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

    int_part = datum >> 10;
    frac = datum & 1023;

    increment = ((uint32_t)slopsix_exp_q11[frac] << (uint32_t)int_part) >> 5;

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
