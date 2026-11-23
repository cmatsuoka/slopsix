/*
 * slopsix_internal.h - internal structures shared by the library sources.
 *
 * Nothing here is public; the only public header is include/slopsix.h.
 */
#ifndef SLOPSIX_INTERNAL_H
#define SLOPSIX_INTERNAL_H

#include "slopsix.h"

/* --------------------------------------------------------------- constants */

/* Voices and operators. */
#define SLOPSIX_VOICE_COUNT 16
#define SLOPSIX_OP_COUNT    6

/* The DX7's algorithm count (the OPS's 32 connection tables). */
#define SLOPSIX_ALGORITHM_COUNT 32

/* The engine's anchor rate; the tables and rate constants derive from it
 * at context creation (research F-04). */
#define SLOPSIX_DEFAULT_SAMPLE_RATE 49096u

/* The DX7 patch dump size, in bytes. */
#define SLOPSIX_VOICE_DUMP_SIZE 156

/* Log-frequency word: the firmware's own 16-bit, 4096-units-per-octave
 * word.  The EGS's frequency field is the same quantity at 1024 units per
 * octave (word >> 2), so the firmware's constants appear unscaled here:
 * the frequency bias is 0x1BA8 and the master tune is `value << 1`.
 * See plan decision D-12. */
#define SLOPSIX_LOG_UNITS_PER_OCTAVE 4096

/* The EGS's 14-bit frequency field, 1024 units per octave. */
#define SLOPSIX_EGS_UNITS_PER_OCTAVE 1024

/* The hardware's frequency bias, subtracted from the log-frequency word
 * before the master tune is added (research P-01). */
#define SLOPSIX_FREQ_BIAS 0x1BA8

/* The voice-frequency word's floor.
 *
 * The firmware clamps its voice-frequency word at zero before it adds
 * the master tune: `clamp(f_note + pitch_EG_level - 0x1BA8, 0, ...)`
 * (research P-01, the firmware's composition chain).  The pitch-EG level
 * there is the absolute 16-bit level word, whose neutral value is 0x4000,
 * so the clamp reads
 *
 *   f_note + level_offset >= 0x1BA8 - 0x4000 = -9304
 *
 * in the engine's 4096-per-octave word.  A voice whose pitch falls below
 * the floor therefore stops at it: the operator's own frequency (the
 * coarse and fine ratios) is added by the EGS afterwards and is not
 * dragged down with the voice word. */
#define SLOPSIX_FREQ_VOICE_FLOOR (-9304)

/* One unit of the Q16 public scales is 65536. */
#define SLOPSIX_Q16_ONE 65536

/* The keyboard level scaling: 43 key-code groups of three semitones
 * each, covering the whole keyboard (research E-05). */
#define SLOPSIX_KLS_GROUPS 43

/* ------------------------------------------------------- generated tables */

/* The generated tables (shared const data, plan decision D-4).  They are
 * defined in src/slopsix_tables.c and produced by the generators under
 * tools/; the checked-in file must stay reproducible from them.
 *
 * Every translation unit that reads a table declares it here, once. */
extern const int16_t slopsix_logsin_quarter[1024];
extern const uint16_t slopsix_exp_q11[1024];
extern const uint8_t slopsix_note_code[128];
extern const uint16_t slopsix_note_word[128];
extern const uint8_t slopsix_com_log[6];
extern const int32_t slopsix_coarse_offset[32];
extern const int16_t slopsix_fine_offset[101];

/* The ROM's tables the modulation paths read (checked in verbatim;
 * src/slopsix_lfo.c). */
extern const uint8_t slopsix_lfo_sin_table[64];     /* the sine LFO's wave */
extern const uint8_t slopsix_amp_mod_table[256];    /* the amp-mod curve */
extern const uint8_t slopsix_pitch_mod_sens[8];     /* the PMS table */

/* --------------------------------------------- the analog tail (the filter) */

/* The analog tail's reconstruction filter (the DX7's own output stage:
 * the board's multi-section low-pass after the DAC and the sample-and-hold
 * circuits, research OU-04/OQ-22; the algorithm page §8.1 has the
 * specification).
 *
 * The DX7's tail models the DAC's zero-order hold and the op-amp network:
 * a gentle high-shelf cut for the sample-and-hold aperture's sinc droop,
 * then a 4th-order cascade of two bilinear (prewarped) biquad low-passes
 * with staggered 16.0/16.5 kHz corners and gentle Q.  It describes its tail
 * as generated section descriptors (kind, corner, Q, shelf gain).
 *
 * The DX7S's tail is the experimental section-descriptor design: the
 * zero-order-hold aperture shelf first, then a 24 dB/oct Butterworth at
 * 21 kHz.  The digital coefficients are designed at the host's rate at
 * context creation with a Vicanek-matched low-pass and an RBJ high-shelf
 * (src/slopsix_filter.c), so there is no Nyquist cramping at 44.1/48 kHz.
 */

/* The cascade's section count. */
#define SLOPSIX_FILTER_SECTIONS 3

/* The signal's fixed point: the DAC's code in Q12, so the analog stage's
 * output keeps a resolution well below the int16 grid (the code's integer
 * grid would quantize the filter's output). */
#define SLOPSIX_FILTER_Q12 12

/* A section descriptor, as the generator emits it: one row of
 *   kind, freq_millihz, q_tenk, gain_millidb
 * (tools/gen_filter_table.py).  The DX7's low-pass kind is bilinear
 * (prewarped); the DX7S's is Vicanek-matched. */
extern const int32_t
    slopsix_filter_desc_dx7[SLOPSIX_FILTER_SECTIONS][4];

/* The section kinds of the descriptor design (generated). */
#define SLOPSIX_FILTER_IDENTITY        0
#define SLOPSIX_FILTER_LOWPASS         1
#define SLOPSIX_FILTER_HIGHSHELF       2
#define SLOPSIX_FILTER_LOWPASS_BILINEAR 3

/* One section's runtime state: the digital coefficients (Q30) and the
 * section's four past values (Q12, the direct form I's states). */
typedef struct {
    int32_t b0;
    int32_t b1;
    int32_t b2;
    int32_t a1;
    int32_t a2;
    int32_t x1;
    int32_t x2;
    int32_t y1;
    int32_t y2;
} slopsix_filter_section;

/* The filter's state: the sections' coefficients and their past values.
 * The filter is a mono stage - the hardware has one, after the mix. */
typedef struct {
    slopsix_filter_section section[SLOPSIX_FILTER_SECTIONS];
} slopsix_filter;

/* The filter's design at a host rate (and its state cleared); one sample
 * through the filter, the DAC's code in and the analog stage's output out
 * (Q12 of a code). */
void slopsix_filter_init(slopsix_filter *filter, uint32_t sample_rate);
/* The design from a section-descriptor table (kind, freq_millihz, q_tenk,
 * gain_millidb).  The DX7 uses the bilinear low-pass; the DX7S uses the
 * Vicanek-matched one; the RBJ high-shelf is shared.  The coefficients are
 * derived at the host's rate. */
void slopsix_filter_init_with(slopsix_filter *filter,
    const int32_t desc[SLOPSIX_FILTER_SECTIONS][4],
    uint32_t sample_rate);
int32_t slopsix_filter_process(slopsix_filter *filter, int32_t code);

/* ------------------------------------------------- operator core (Phase 2) */

/* The OPS's phase adder is 23 bits wide and the sine circuit consumes
 * its top 12 bits, so one sine period spans 2^23 accumulator units
 * (research O-01).  The accumulator state is a 32-bit
 * word masked to the adder's width at every increment (plan D-6). */
#define SLOPSIX_PHASE_BITS 23
#define SLOPSIX_PHASE_MASK 0x007FFFFFu
#define SLOPSIX_SINE_PHASE_SHIFT 11
#define SLOPSIX_SINE_PHASE_MASK 0x0FFFu

/* The sine phase's fields: the top bit is the wave half (the output's
 * sign), the next one selects the quarter (the index is complemented in
 * the second quarter) and the low ten bits are the quarter-wave ROM
 * index. */
#define SLOPSIX_SINE_HALF_BIT 0x800u
#define SLOPSIX_SINE_QUARTER_BIT 0x400u
#define SLOPSIX_SINE_INDEX_MASK 0x3FFu
#define SLOPSIX_SINE_INDEX_MAX 1023u

/* The amplitude domain: the hardware's 12-bit Q8 log2 value, where the
 * linear gain is 2^(value / 256) (research O-03).
 * The log-sine table is Q10 (1024 units per octave), so the amplitude
 * value enters the signal exponent shifted by two bits. */
#define SLOPSIX_AMP_Q8_SHIFT 2

/* The modulation deviation scale.  A modulator's linear output is added
 * directly to the modulated operator's 12-bit sine phase index, so one
 * full-scale 14-bit modulator (16383) deviates the phase by about four
 * periods (research O-05; a bit-synchronous OPS model).  The engine's
 * phase word carries the sine index in its top 12 bits, so the shift is
 * the sine index's position.  Modulators are not clamped: the hardware
 * adds the operator sum as it is. */
#define SLOPSIX_MOD_DEVIATION_SHIFT 11

/* The feedback shifter's deepest position.  The die carries a 0..6-bit
 * feedback shifter (research O-06); the level-to-shift mapping is
 * provisional and documented in the constant block below. */
#define SLOPSIX_FB_SHIFT_MAX 6

/* The word for MIDI note 69 (A4): the note word's reference pitch, and
 * the anchor of the frequency calibration (see src/slopsix_pitch.c). */
#define SLOPSIX_WORD_A4 23208

/* log2(440 Hz) in Q10 units (1024 per octave): 8.781743 octaves. */
#define SLOPSIX_LOG2_440_Q10 8993

/* The EGS's datum for MIDI note 69, i.e. the 1024-per-octave value whose
 * frequency exponent gives the closest frequency to 440 Hz that the
 * hardware can produce at 49,096 Hz: 440.12 Hz (see src/slopsix_pitch.c). */
#define SLOPSIX_EGS_DATUM_A4 10443

/* --------------------------------------------------- hypothesis-grade block
 *
 * Every constant whose value comes from an emulator-derived model rather
 * than from the hardware or the firmware lives here.  Each entry carries
 * its evidence grade; a later capture replaces a constant, never a code
 * path.
 */

/* Evidence grade: emulator-derived model (research P-02).
 * The coarse and fine ratio offsets, in the engine's 4096-per-octave
 * word.  Generated by tools/gen_freq_table.py; declared with the other
 * generated tables above. */


/* Evidence grade: emulator-derived model (research O-06).
 * The feedback level-to-shift mapping: the OPS's feedback field is
 * 3 bits (0..7, level 0 = no feedback) and the die's shifter has
 * 0..6 positions.  The mapping used here is shift = 7 - level, i.e. the
 * highest feedback level divides the least; a bit-synchronous OPS model
 * agrees on the effective division. */
#define SLOPSIX_FB_SHIFT(level) \
    (SLOPSIX_FB_SHIFT_MAX + 1u - (unsigned)(level))

/* Evidence grade: die's algorithm-box walkthrough (research A-01, A-02).
 * The sum/delay register: the walk's one per-voice register keeps its
 * content where C is set, and loads or adds the current operator's
 * signal where D is set; the selector reads either the register's new
 * value (SEL2), the value before the producer's update (SEL3), the
 * feedback pair's last pushed value (SEL4) or its average (SEL5).  The
 * ROM's first control bit is the feedback pair's store enable: unlike
 * the earlier bus model, no operator's output is a member of the path
 * without its own bit - in the self-feedback loops the reading operator
 * pushes its own signal (its row carries the bit), while in algorithms 4
 * and 6 the pair holds operator 4's resp. operator 5's values only. */

/* Evidence grade: bit-synchronous OPS model (research OU-01), matching
 * the output-quantization section of the algorithm page.  The per-voice
 * compander's shift selection: the 14-bit voice word's magnitude decides
 * how many low bits the transmitted 12-bit mantissa drops - three at or
 * above 0x1000, two at or above 0x800, one at or above 0x400, none
 * below.  The word's value is the mantissa shifted back up: the dropped
 * low bits are the quantization error.
 *
 * The exact SF-code numbering and the overflow behaviour of the
 * hardware's shifter are open (research OU-02); the engine pins the
 * overflow as a clamp to the mantissa's 12-bit two's-complement window,
 * which keeps the word's value inside the 14-bit range a single voice's
 * output can reach. */
#define SLOPSIX_COMPANDER_THRESHOLD_1 0x400u
#define SLOPSIX_COMPANDER_THRESHOLD_2 0x800u
#define SLOPSIX_COMPANDER_THRESHOLD_3 0x1000u
#define SLOPSIX_COMPANDER_SHIFT_MAX 3u
#define SLOPSIX_COMPANDER_MANTISSA_MAX 2047
#define SLOPSIX_COMPANDER_MANTISSA_MIN (-2048)

/* Evidence grade: the engine's V1 model of an open analog question
 * (research OU-02, OQ-24).  The mixed bus's DAC stage reduces the bus to
 * the hardware's 12-bit DAC code grid.  The bus's unit is a quantized
 * voice's value (mantissa << shift), whose full scale is 8 * 2047, so the
 * code grid sits at one eighth of the bus unit - the same domain the
 * compander's 12-bit datum occupies.
 *
 * The step does not saturate.  The 12-bit DAC converts one voice per TDM
 * slot and the voices sum in the analog domain after conversion
 * (algorithm page §8); the per-voice 12-bit window is already the
 * compander's clamp.  The bus is therefore the analog-domain sum, which
 * legitimately exceeds one voice's window on a chord; the analog stage's
 * rail is the int16 conversion's saturation, downstream. */
#define SLOPSIX_DAC_GRID_SHIFT 3u

/* The DX7S transmitted DAC word (src/dx7s/slopsix_output.c): the
 * mixed bus's sum divided by sixteen, on the 15-bit DA2-DA16 rails.
 * The OPS2 emits the DAC code on fifteen lines (the IC pin list's
 * "digital code for analog convert"; the PCM54HP's top Bit 1 runs
 * grounded on the Overall Circuit Diagram), so the transmitted word is
 * 15-bit linear, and sixteen voice windows (the sum of sixteen
 * full-scale voices, 262016 bus units) land on 16376 - just inside the
 * rails - when the accumulator's sum is divided by sixteen.  The DAC
 * word never saturates for any polyphony the engine can sum; the
 * machine's analog side (the scaled hold legs and the volume VCA)
 * restores the line level outside the model.
 *
 * Evidence grade: hypothesis-grade (D7S-13, 2026-11-21).  The pin
 * evidence pins the word's width and linearity; the divide-by-sixteen
 * is what sixteen voice windows require to land inside the word with
 * zero overflow, and the analog restore gain stays out of the model
 * (research OQ-24/OU-04). */
#define SLOPSIX_DX7S_DA_SHIFT 4u
#define SLOPSIX_DX7S_DA_MAX 16383
#define SLOPSIX_DX7S_DA_MIN (-16384)

/* The DX7S conversion's scale and analog ceiling
 * (src/dx7s/slopsix_output.c).
 *
 * The default scale is the DAC code times eight (12 signal bits of Q12
 * plus 16 gain bits, three dropped): the machine's analog restore gain
 * relative to the transmitted word, landing the delivered difference
 * at the real-device figure (about -6 dB per full-scale voice vs the
 * Mk1's delivered reference, one voice at 8184).
 *
 * The ceiling is the board's output swing (the reconstruction filter,
 * then the volume VCA and the output amplifier): the delivered int16
 * rail is where this model places it at the restore calibration.  The
 * knee sits at 24564 (three stacked Mk1 windows), above which the
 * value compresses toward the rail with a rational soft clip -
 * continuous with unit gain at the knee, asymptotic below the rail -
 * so a hot stack's top is limited the way an output amp approaches its
 * swing, and the digital flat-top never occurs.  The real machine's
 * analog ceiling carries more room above its line level than a 16-bit
 * deliverable at this calibration does; the curve is what the int16
 * delivery keeps of that headroom.
 *
 * Evidence grade: hypothesis-grade (D7S-13, 2026-11-21).  The -6 dB
 * delivered gain is the maintainer's pin from the real-device
 * measurement; the knee's placement and the curve stand in for the
 * output stage's saturation, which no capture pins yet. */
#define SLOPSIX_DX7S_OUTPUT_SCALE_SHIFT 25
#define SLOPSIX_DX7S_OUTPUT_SATURATION_KNEE 24564
#define SLOPSIX_DX7S_OUTPUT_RAIL 32767

/* The normalized DX7S scale: the DAC code times sixteen, twice the
 * default's times eight - one bit of gain (+6 dB) that cancels the
 * default's -6 dB against the Mk1, so the same patch delivers at the
 * DX7 Mk1's level (one full-scale DX7S voice at 16368 instead of
 * 8184).  The analog ceiling still applies: content past the knee at
 * 24564 compresses toward the rail, so a hot stack loses more of its
 * top here than in the default stage.  Switch the stage at runtime
 * through the normalize_output flag; the per-sample path only selects
 * one of the two constants. */
#define SLOPSIX_DX7S_NORMALIZED_SCALE_SHIFT 24

/* The pitch envelope (Phase 4).  It is CPU-side and per voice, updated
 * every second OCF tick (about 187.6 Hz), as the firmware does (research
 * E-04, plan decision D-1).  The level is held in the firmware's 16-bit
 * domain, where the patch's level quantizes through the ROM's table and
 * the neutral value is 0x4000.
 *
 * The envelope itself carries no cadence: the update is driven by the
 * context's global toggle (below), so every voice's envelope steps on the
 * same samples, as on the hardware. */
typedef struct {
    uint8_t increment[4];   /* the ROM rate table's increments, 1..255 */
    int32_t target[4];      /* the ROM level table's words: value << 7 */
    int32_t level;          /* the current level, 0..0x7F80 */
    uint8_t step;           /* 0..2 held stages, 3 sustain, 4 release,
                             * 5 finished */
#if defined(SLOPSIX_DX7S)
    /* The DX7S's composition of this envelope (D7S-4): the range's span
     * divisor and the velocity switch's depth factor.  Only the DX7S's
     * own law (src/dx7s/slopsix_pitch_eg.c) reads and writes them, and
     * the DX7's law ignores them entirely, so a DX7 context plays the
     * firmware's envelope whatever they hold.
     *
     * The neutral values are the zeroed ones, so the memset in
     * slopsix_pitch_eg_init leaves an envelope the DX7S has not
     * composed at the Mk1's excursion: a range shift of zero divides
     * nothing (the eight-octave range is the Mk1's own span), and the
     * velocity switch off leaves the depth alone (the depth factor is
     * read only while the switch is on). */
    uint8_t range_shift;        /* 0 = full span, else the divisor's log2 */
    uint8_t velocity_on;        /* the velocity switch */
    uint16_t velocity_scale_q8; /* 0..256: the depth factor, 256 = full */
#endif
} slopsix_pitch_eg;

/* The global control-rate cadence.
 *
 * The pitch EG and the LFO are both free-running control-rate processes
 * driven by the same OCF timer, so they share one accumulator: the host
 * rate, the sub-tick accumulator and the accumulator's per-sample step (a
 * multiple of the anchor rate).  The two processes differ only in that
 * step - the LFO's is twice the pitch EG's - so one gate serves both and
 * their phases stay locked by construction. */
typedef struct {
    uint32_t sample_rate;
    uint32_t acc;           /* the sub-tick cadence accumulator */
    uint32_t step;          /* the accumulator's increment per sample */
} slopsix_cadence;

/* The cadence's period numerator.
 *
 * The serial rate is the 9.4265 MHz master clock divided by 192, so a
 * sample spans 192 master clocks.  The OCF timer reloads 3140 E-clock
 * cycles and the E-clock is the master clock divided by 8, so one tick
 * spans 8 * 3140 = 25,120 master clocks.  At the engine's anchor rate
 * that is 25,120 / 192 = 785 / 6 samples per tick; the pitch EG advances
 * on every second tick, so its step is half the LFO's.  For a host rate
 * the same real-time interval is 785 * rate / (6 * 49,096) samples, which
 * the accumulator realizes in 32-bit integer arithmetic. */
#define SLOPSIX_CADENCE_TICK_NUM 785u

static inline void slopsix_cadence_init(slopsix_cadence *cadence,
    uint32_t sample_rate, uint32_t step)
{
    cadence->sample_rate = sample_rate;
    cadence->acc = 0u;
    cadence->step = step;
}

/* The cadence gate: call once per sample; it returns nonzero on the
 * samples where the process runs.  The gate advances the accumulator only.
 *
 * The rate is capped like the frequency path's, so the products stay
 * inside 32 bits. */
static inline int slopsix_cadence_tick(slopsix_cadence *cadence)
{
    uint32_t rate = cadence->sample_rate;
    uint32_t threshold;

    if (rate == 0u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    }
    if (rate > SLOPSIX_DEFAULT_SAMPLE_RATE * 8u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE * 8u;
    }

    threshold = SLOPSIX_CADENCE_TICK_NUM * rate;
    cadence->acc += cadence->step;

    if (cadence->acc >= threshold) {
        cadence->acc -= threshold;
        return 1;
    }

    return 0;
}

/* The pitch envelope's update cadence.
 *
 * The firmware's update is driven by a global toggle, not by a per-voice
 * one: the pitch EG process runs every second OCF tick for the whole
 * instrument, so every voice's envelope steps on the same samples.  The
 * accumulator therefore belongs to the context, not to the voice; a voice
 * that starts between two ticks waits for the next one, exactly as on the
 * hardware. */
typedef slopsix_cadence slopsix_pitch_eg_cadence;

/* The pitch EG advances on every second OCF tick, 50,240 master clocks,
 * i.e. 785 / 3 samples at the anchor rate: half the LFO's step. */
#define SLOPSIX_PITCH_EG_TICK_ACC (3u * SLOPSIX_DEFAULT_SAMPLE_RATE)

static inline void slopsix_pitch_eg_cadence_init(
    slopsix_pitch_eg_cadence *cadence, uint32_t sample_rate)
{
    slopsix_cadence_init(cadence, sample_rate, SLOPSIX_PITCH_EG_TICK_ACC);
}

/* The cadence gate for the pitch-EG process.
 *
 * The gate advances the cadence only: it never touches an envelope, so it
 * cannot advance one twice.  The caller advances every sounding voice's
 * envelope with slopsix_pitch_eg_update() on the samples where the gate
 * fires, which is what makes every voice's staircase land on the same
 * samples, as on the hardware. */
static inline int slopsix_pitch_eg_cadence_tick(
    slopsix_pitch_eg_cadence *cadence)
{
    return slopsix_cadence_tick(cadence);
}

/* The pitch-EG level's neutral word: the firmware initializes an idle
 * voice's level to 0x4000, so the level's contribution to the frequency
 * word is its distance from that value. */
#define SLOPSIX_PITCH_EG_NEUTRAL_WORD 0x4000

/* ------------------------------------------------------------- LFO state */

/* The LFO is CPU-side and global to the instrument (research E-04/L-01,
 * plan decision D-1): the hardware's interrupt handler advances the LFO
 * and pushes the two modulation registers once per OCF tick, about
 * 375.3 Hz, so both register staircases step at that cadence.
 *
 * All state fields mirror the firmware's own variables; the patch block's
 * activated values (the two increments, the depths, the sensitivity and
 * the waveform) are stored with the runtime state, exactly as the
 * firmware's activation writes its registers at patch load. */
typedef struct {
    /* The activated parameters (the ROM's own scaling at patch load). */
    uint16_t phase_increment;   /* the speed law's 16-bit increment */
    uint16_t delay_increment;   /* the delay law's 16-bit increment */
    uint8_t waveform;           /* the raw wave byte, 0..5 (+ fall-through) */
    uint8_t pitch_mod_depth;    /* (660 * PMD) >> 8 */
    uint8_t amp_mod_depth;      /* (660 * AMD) >> 8 */
    uint8_t pitch_mod_sens;     /* the PMS table's value, 0..255 */

    /* The runtime state. */
    uint16_t phase;             /* the 16-bit phase accumulator */
    uint16_t delay_acc;         /* the 16-bit delay accumulator */
    uint8_t fade;               /* the delay's fade-in factor, 0..255 */
    uint8_t sh_acc;             /* the S&H wave's LCG holder */
    int8_t amplitude;           /* the current 8-bit wave value, signed */

    /* The per-tick push values. */
    uint8_t amp_mod_register;   /* the value at the EGS's amp-mod register */
    int16_t pitch_mod_term;     /* the wave's own share of the pitch word */
    int16_t pitch_mod_word;     /* term + bend: the register's 16-bit word */
} slopsix_lfo;

/* The LFO's update cadence.
 *
 * The hardware's one OCF timer drives everything: the LFO advances (and
 * both registers are pushed) on every tick, the pitch EG walks on every
 * second tick.  Both share slopsix_cadence; the LFO's step is twice the
 * pitch EG's, so the pitch EG's gate lands exactly on every second LFO
 * tick, as on the hardware (see the pitch EG's cadence above). */
typedef slopsix_cadence slopsix_lfo_cadence;

/* The LFO ticks on every OCF tick, 25,120 master clocks, i.e. 785 / 6
 * samples at the anchor rate. */
#define SLOPSIX_LFO_TICK_ACC (6u * SLOPSIX_DEFAULT_SAMPLE_RATE)

static inline void slopsix_lfo_cadence_init(slopsix_lfo_cadence *cadence,
    uint32_t sample_rate)
{
    slopsix_cadence_init(cadence, sample_rate, SLOPSIX_LFO_TICK_ACC);
}

/* The cadence gate: call once per sample; it returns nonzero on the
 * samples where the firmware's OCF handler runs the LFO. */
static inline int slopsix_lfo_cadence_tick(slopsix_lfo_cadence *cadence)
{
    return slopsix_cadence_tick(cadence);
}

/* ------------------------------------------------------------ voice state */

typedef enum {
    SLOPSIX_VOICE_FREE = 0,
    SLOPSIX_VOICE_ON,
    SLOPSIX_VOICE_RELEASED
} slopsix_voice_state;

/* The firmware's reset value for the three voice pitch buffers: the note
 * word for note 36 (C2), 0x2EA8 in the 4096-per-octave domain.  The
 * buffers are valid before the first note, so a glide always has a
 * well-defined starting point. */
#define SLOPSIX_PORTA_RESET_WORD 0x2EA8

typedef struct {
    slopsix_voice_state state;
    bool held;          /* key still down; false while pedal-sustained */
    uint8_t note;       /* 0..127 */
    uint8_t velocity;   /* 0..127 */
    int8_t transpose;   /* the patch transpose keyed with the note */
    uint32_t seq;       /* the voice allocation's sequence counter; the
                         * stealing picks the lowest (the oldest note-on) */

    /* The firmware's portamento buffers (research P-06): this voice's
     * current and target log-frequency words and, when glissando is on,
     * the semitone-quantized word the EGS actually hears.  They are in
     * the engine's 4096-per-octave word and carry the hardware's two
     * zero LSBs; the note word replaces the plain note in the frequency
     * path while a glide runs. */
    int32_t porta_current;
    int32_t porta_target;
    int32_t gliss_current;

#if defined(SLOPSIX_DX7S)
    /* The DX7S unison detune's contribution to this voice's pitch, in
     * the engine's 4096-per-octave word.  It is provisional evidence
     * (D7S-10, OQ-DX7S-05): the unison spread constant below names the
     * law, and the field is zero outside the unison modes and in the
     * DX7 build. */
    int32_t unison_offset;
#endif
} slopsix_voice;

/* --------------------------------------------------------- the live patch */

/* The dump's byte layout: the firmware's own patch buffer (research V-01,
 * offsets firmware-confirmed against PATCH_GET_PTR_TO_SELECTED_OP and the
 * PATCH_* symbols of the listing).  Six operator blocks of 21 bytes come
 * first and the global section follows at 126; bytes 145..154 are the
 * patch name (a host concern) and byte 155 is the serialiser's per-op
 * enable extension, which the engine takes as-is and reads none of (op
 * on/off is not a dump parameter here - see the library page §3).
 *
 * The operator blocks are serialised operator 6 first: block 0 holds
 * operator 6 and block 5 holds operator 1.  That mapping is the one both
 * the firmware's own activation loop (selected operator 0..5 straight
 * into the EGS's operator registers) and the algorithm walk agree with,
 * and it is what makes a sounding INIT voice - whose only non-zero
 * output level sits in its last block - come out of algorithm 1's
 * operator 1. */
#define SLOPSIX_DUMP_OP_BLOCK 21
#define SLOPSIX_DUMP_PEG_RATE 126
#define SLOPSIX_DUMP_PEG_LEVEL 130
#define SLOPSIX_DUMP_ALGORITHM 134
#define SLOPSIX_DUMP_FEEDBACK 135
#define SLOPSIX_DUMP_OSC_SYNC 136
/* Bytes 137..143 are the LFO block: speed 137, delay 138, pitch-mod
 * depth 139, amp-mod depth 140, sync 141, waveform 142, pitch-mod
 * sensitivity 143.  The block's activation (its scaling and its note-on
 * read-back) is the LFO's own (src/slopsix_lfo.c); byte 144 is the key
 * transpose, as its value plus 24. */
#define SLOPSIX_DUMP_LFO_SPEED 137
#define SLOPSIX_DUMP_LFO_DELAY 138
#define SLOPSIX_DUMP_LFO_PMD 139
#define SLOPSIX_DUMP_LFO_AMD 140
#define SLOPSIX_DUMP_LFO_SYNC 141
#define SLOPSIX_DUMP_LFO_WAVE 142
#define SLOPSIX_DUMP_LFO_PMS 143
#define SLOPSIX_DUMP_TRANSPOSE 144

/* One operator's parsed patch data.  The values keep the hardware's own
 * ranges; out-of-contract dump bytes are clamped here, once, so the
 * render and the note-on path never see one. */
typedef struct {
    uint8_t rates[4];       /* the four stage rates, 0..99 */
    uint8_t levels[4];      /* the four stage levels, 0..99 */
    uint8_t rate_scaling;   /* the hardware's 3-bit field, 0..7 */
    uint8_t velocity_sens;  /* the hardware's 3-bit field, 0..7 */
    uint8_t ams_sens;       /* the dump's 2-bit field, 0..3; the DX7S's
                             * additional voice data widens it to 0..7 */
    uint8_t out_level;      /* the operator output level, 0..99 */
    uint8_t mode;           /* 0 = ratio, 1 = fixed frequency */
    uint8_t coarse;         /* 0..31 */
    uint8_t fine;           /* 0..99 */
    uint8_t detune;         /* 0..14 (the hardware's 4-bit signed value) */
    int8_t kls[SLOPSIX_KLS_GROUPS];
    /* the keyboard level scaling's offsets, built at patch-load time
     * exactly as the firmware builds its 6 x 43 RAM curve
     * (research E-05) */
} slopsix_patch_op;

/* The live, parsed patch.  This is the engine's patch-activation state:
 * a patch load re-parses it, and a note-on composes the per-voice values
 * from it.  Sounding voices keep the values they were keyed with, as the
 * chips' registers do. */
typedef struct {
    uint8_t algorithm;      /* the graph index, 0..31 */
    uint8_t feedback;       /* the OPS's 3-bit field, 0..7 */
    uint8_t osc_sync;       /* reset the operator phases at key-on */
    int8_t transpose;       /* semitones, -24..+24; the dump byte minus 24 */
    slopsix_patch_op ops[SLOPSIX_OP_COUNT];  /* index 0 == operator 1 */
    uint8_t peg_rates[4];   /* the pitch EG's stage rates, 0..99 */
    uint8_t peg_levels[4];  /* the pitch EG's stage levels, 0..99 */
    /* The LFO block's own bytes (the dump's 137..143); the activations
     * (the two increments, the depths, the sensitivity and the waveform)
     * live in the context's LFO state, exactly as the firmware's
     * activation ships them to its registers at patch load.  These two
     * stay raw because the note-on path reads them from the patch buffer,
     * not from a register (research fw VOICE_ADD). */
    uint8_t lfo_sync;       /* the dump's LFO SYNC byte, any value != 0 syncs */
    uint8_t lfo_delay;      /* the dump's LFO DELAY byte, 0..99 */
#if defined(SLOPSIX_DX7S)
    /* DX7S-only patch parameters.  They are not part of the 156-byte Mk1
     * dump (plan D7S-6/D7S-11); they live here so a patch load leaves the
     * API-set values alone.  D7S-4 gives them their composition. */
    uint8_t peg_range;        /* SLOPSIX_PEG_RANGE_*, 0..3 */
    uint8_t peg_velocity;     /* 0 = off, 1 = on */
    uint8_t peg_rate_scaling; /* 0..7 */
#endif
} slopsix_patch_state;

#if defined(SLOPSIX_DX7S)
/* The DX7S's key modes and pitch-EG range are the public enums
 * slopsix_key_mode and slopsix_peg_range (include/slopsix.h): the
 * personality's own state uses those values, and the DX7's `mono`
 * bookkeeping stays untouched.  8 octaves is the Mk1-equivalent range
 * default, so a neutral DX7S context reproduces the Mk1 pitch-EG span;
 * it is the encoding's raw 0 as well, so the default, the zeroed
 * envelope and the encoded value coincide. */
#define SLOPSIX_PEG_RANGE_DEFAULT     SLOPSIX_PEG_RANGE_8_OCTAVES

/* The DX7S unison groups (D7S-3/D7S-4): Unison Poly is four fixed
 * four-voice groups at voice indices 0..3, 4..7, 8..11, 12..15; Unison
 * Mono is one sixteen-voice group.  Fixed groups keep the allocation
 * deterministic and leave the render loop walking all sixteen voices. */
#define SLOPSIX_DX7S_UNISON_GROUPS       4u
#define SLOPSIX_DX7S_UNISON_POLY_VOICES  4u

/* The provisional unison detune spread (D7S-10, OQ-DX7S-05).
 *
 * Evidence grade: working model.  The owner's manual confirms the 0..7
 * parameter in the two unison modes but gives no law.  Each voice's
 * requested distance from the stack's centre is `2*rank - (size-1)`
 * ranks, each rank is `detune` steps, and a step is one engine word
 * (1/4096 octave, about 0.29 cent).  Value 0 is the neutral default, so
 * Poly and a zeroed detune leave the Mk1 parity intact.
 *
 * The requested step is finer than the EGS frequency field, so the
 * offset is composed in whole field units, rounded to the nearest one
 * (src/dx7s/slopsix_voice.c): the field's floor division would otherwise
 * render the spread biased flat-downward.  One rendered unit is
 * SLOPSIX_DX7S_UNISON_GRID_WORD engine words, the same grid and step the
 * per-operator detune uses (P-04).  At detune 7 a four-voice stack
 * renders about ±6 cents and a sixteen-voice stack about ±31 cents.  A
 * later capture replaces the law, not the code path. */
#define SLOPSIX_DX7S_UNISON_DETUNE_STEP_WORD 1
/* One EGS frequency-field unit in engine words (D-16, P-04): the grid a
 * voice word's phase increment is quantized onto. */
#define SLOPSIX_DX7S_UNISON_GRID_WORD       4u
#endif

/* The note the firmware hands to its note-table conversion: the dump's
 * transpose byte counts from 24, so its neutral value is 24 and the
 * composed note is the key plus the transpose.  The firmware clamps the
 * composed note at 127; the engine clamps it at zero too (the note
 * table's safe default, T[0] = 0), where the firmware's own 8-bit
 * arithmetic would wrap past the table. */
static inline int32_t slopsix_effective_note(uint8_t note, int8_t transpose)
{
    int32_t composed = (int32_t)note + (int32_t)transpose;

    if (composed > 127) {
        composed = 127;
    }
    if (composed < 0) {
        composed = 0;
    }
    return composed;
}

/* -------------------------------------------- envelope and level (Phase 3) */

/* The amplitude domain is the hardware's 12-bit Q8 log2 domain: 256 units
 * per octave (research O-03).  The engine composes every level in that
 * domain and the operator core converts once, at the signal exponent.
 *
 * The level parameters (the operator output level and the envelope's
 * stage levels) are hardware parameters in the 0..99 range; the engine
 * maps them onto a "microstep" gain of 0..127, where one microstep is
 * 1/8 of an octave, i.e. 32 units of the Q8 domain.  The mapping is
 * closed (research E-08, the ROM's own attenuation table is the exact
 * inverse of the law below) and is implemented in
 * src/slopsix_eg.c. */
#define SLOPSIX_MICROSTEP_MAX 127
#define SLOPSIX_MICROSTEP_Q8 32
#define SLOPSIX_MICROSTEP_Q8_SHIFT 5

/* The velocity's attenuation unit: half a microstep, because the
 * hardware's operator volume register holds the keyboard level scaling's
 * part twice (research E-05, research E-07). */
#define SLOPSIX_VELOCITY_Q8_SHIFT 4

/* The reference offset of the level composition: the Q8 domain's zero
 * point, so that a voice at the maximum level sits at the origin of the
 * domain.  The value is the model's domain-conversion constant
 * (research O-07); it is the only place where the absolute level of the
 * engine is anchored.
 *
 * 4000 is calibrated against VDX7's raw operator signal: at the earlier
 * 4256 the engine's operator output was one octave low, which halved the
 * modulation index (the compander sits downstream and does not cancel
 * it).  With 4000 the raw signals match VDX7 and the 64-patch median
 * spectral distance drops from 0.434 to 0.058 (2026-11-18). */
#define SLOPSIX_LEVEL_OFFSET_Q8 4000

/* The envelope's own fixed point: the EGS attenuation with 16
 * fractional bits for the block cadence's interpolation.  The level is
 * held in the hardware's inverted 12-bit attenuation domain (0 is full
 * level, 0xFF0 is the model's silence floor); the render path converts
 * it to the engine's gain direction with SLOPSIX_EG_GAIN_BASE. */
#define SLOPSIX_EG_FRAC 16

/* The EGS attenuation domain.  0xFF0 is the target/level clamp ("minimum
 * level is 4096 - 16").  The attack jumps a louder envelope to 0x94C
 * (4096 - 1716) before it slopes towards its target (research E-01). */
#define SLOPSIX_EG_ATT_MAX 0xFF0
#define SLOPSIX_EG_ATT_JUMP 0x94Cu

/* The gain direction's base: attenuation A is gain (4032 - A) in the
 * engine's Q8 domain.  4032 is 63 level-register steps of 64 units, the
 * largest target the EGS composition produces. */
#define SLOPSIX_EG_GAIN_BASE 4032

/* The measured static durations are gone with the msfa model; the EGS
 * target composition's smallest operator volume register value is 4,
 * which makes 0x40 the smallest attenuation a target can carry. */
#define SLOPSIX_EG_OP_LEVEL_MIN 4

/* The envelope's block cadence: the EGS model advances every sample,
 * but the diagnostic block cadence runs 64 steps at a time and
 * interpolates the level across the block (plan decision D-9). */
#define SLOPSIX_EG_BLOCK 64

/* The render path's per-block, operator-major evaluation (the DX7
 * render path's shape, matching the reference emulator's 64-sample
 * operator blocks).  The block renderer evaluates one operator over a
 * whole block of samples before moving to the next; a process call is
 * cut into sub-blocks that end on the control-rate ticks, and every
 * sub-block renders in blocks of this size at most. */
#define SLOPSIX_RENDER_BLOCK 64

/* One operator's envelope generator.
 *
 * The EGS model (algorithm page §5.1): the level lives in the chip's
 * inverted 12-bit attenuation domain, the stage machine walks
 * S0 -> S1 -> S2 while the key is held and jumps to S3 at key-up, and
 * the rate gate clocks the movement from the 16-bit master clock.  The
 * render path converts the attenuation to the engine's gain direction.
 */
typedef struct {
    /* The parameters, set at note-on. */
    uint8_t rate_reg[4];    /* the EGS rate registers, (rate*164)>>8 */
    uint8_t level_reg[4];   /* the EGS attenuation levels, 0..63 */
    uint8_t op_level;       /* the EGS operator-volume register, 4..255 */
    uint8_t rate_scaling;   /* the note's rate scaling delta, 0..31 */

    /* The runtime state (attenuation << SLOPSIX_EG_FRAC). */
    int32_t level;          /* the current attenuation */
    int32_t target;         /* the current stage's target attenuation */
    uint8_t stage;          /* 0..3, 4 when the envelope has finished */
    uint8_t active;         /* the envelope is in use */
    uint8_t rising;         /* the stage lowers the attenuation */
    uint8_t down;           /* the key is down */
    uint8_t compress;       /* the stage-0 delay compression flag: set
                             * by a stage-0 key-off (the previous stage-0
                             * delay did not complete), cleared by a
                             * completed stage 0, as the model does */
    uint8_t nshift;         /* the slow-rate clock shift */
    uint8_t pshift;         /* the fast-rate movement shift */
    uint8_t mask;           /* the fractional-qrate clock mask */
    uint16_t small;         /* the slow-rate clock mask */
    uint16_t clock;         /* the envelope's 16-bit master clock,
                             * free-running at the engine's anchor rate
                             * and reset at note-on (the model's one
                             * shared master clock never resets; a key
                             * event realigning the gate's phase within
                             * one gate period is the engine's documented
                             * deviation - slopsix_eg_clock) */
    uint32_t tick_acc;      /* the master clock's sub-tick accumulator */
    uint32_t tick_threshold; /* the host rate the accumulator crosses */

    /* The diagnostic block cadence (plan decision D-9). */
    uint8_t block_mode;     /* 1: step once per block and interpolate */
    uint8_t block_pos;      /* the sample position inside the block */
    int32_t block_level;    /* the block's starting attenuation */
    int32_t block_step;     /* the block's per-sample attenuation step */
} slopsix_eg;

/* ------------------------------------------------------- operator (Phase 2) */

/* One operator's runtime state.  The phase accumulator is free-running
 * and never reset at note-on (the hardware's note-on phase behaviour is
 * open, research O-01). */
typedef struct {
    uint32_t phase;    /* the 23-bit phase accumulator */
    uint32_t increment; /* the 22-bit phase increment per sample */
} slopsix_operator;

/* One operator's static parameters.  The frequency composition reads
 * mode, coarse, fine and detune at note-on time; the render path reads
 * only log_amp_q10, and the envelope composes the effective amplitude
 * from it (or from the envelope's own level) and the algorithm's
 * compensation. */
typedef struct {
    uint8_t mode;        /* 0 = ratio, 1 = fixed frequency */
    uint8_t coarse;      /* 0..31 */
    uint8_t fine;        /* 0..99 */
    uint8_t detune;      /* 0..14 (the hardware's 4-bit signed value) */
    int32_t log_amp_q10; /* the operator's log amplitude, already shifted
                          * into the log word's Q10 scale; whoever fills
                          * the struct must apply `<< SLOPSIX_AMP_Q8_SHIFT`.
                          * The resolution is the hardware's Q8 (the value
                          * is a 12-bit word): the envelope-less render
                          * path shifts back to Q8, so no finer detail
                          * survives */

    /* The level pipeline's parameters (Phase 3). */
    uint8_t out_level;     /* the operator output level, 0..99 */
    uint8_t velocity_sens; /* the key velocity sensitivity, 0..7 */
    uint8_t ams_sens;      /* the amplitude modulation sensitivity, 0..3
                            * (0..7 on the DX7S) */
    int8_t kls[SLOPSIX_KLS_GROUPS]; /* the keyboard level scaling offsets,
                                     * one per key-code group */
    /* The amplitude modulation's reduction, in the Q8 domain (a
     * negative quantity the render path subtracts from the operator's
     * log amplitude).  The EGS's amp-mod register changes only on the
     * LFO's ticks, so the reduction is composed then (and at note-on)
     * and the render path only reads it. */
    int32_t ams_reduction_q8;
} slopsix_op_params;

/* ------------------------------------------------- the operator graph */

/* Where an operator's modulation input comes from.  The DX7
 * configuration uses the hardware's five selector sources (research
 * A-01); the general graph leaves room for more. */
typedef enum {
    SLOPSIX_MOD_NONE = 0,
    SLOPSIX_MOD_OPERATOR,          /* the operator computed just before */
    SLOPSIX_MOD_FEEDBACK,          /* the feedback path's averaged value */
    SLOPSIX_MOD_REGISTER,          /* the sum/delay register's new value */
    SLOPSIX_MOD_REGISTER_DELAYED,  /* the register's value before the
                                    * producer's own update */
    SLOPSIX_MOD_FEEDBACK_DELAYED   /* the feedback path's last stored
                                    * value */
} slopsix_mod_source;

/* One operator's connection in the graph.  The selector (`source`) is
 * stored on the row that produces the next row's modulation - the entry
 * in the ROM column labelled with this operator; the compensation and
 * carrier fields belong to the consumer, operator N-1.  C/D/A configure
 * the registers this operator's own row drives: C/D the sum/delay
 * register (`mren`), A the feedback pair */
typedef struct {
    slopsix_mod_source source;
    uint8_t source_op;  /* the selected signal's operator, 1..6, when
                         * relevant */
    uint8_t carrier;    /* the output joins the voice's carrier sum */
    uint8_t com;        /* the carrier-count compensation code, 0..5 */
    int32_t com_q8;     /* the code's compensation in the Q8 domain: the
                         * negative value the carrier's log amplitude
                         * carries.  It is algorithm data, so it is
                         * decoded once with the algorithm's table and
                         * only read by the render path. */
    uint8_t ctrl_c;     /* the sum/delay register: 1 keeps its content */
    uint8_t ctrl_d;     /* the sum/delay register: 1 loads the signal */
    uint8_t fb_out;     /* the output is stored into the feedback pair */
} slopsix_op_conn;

/* The block renderer's per-algorithm schedule (decoded once with the
 * algorithm's connection data).
 *
 * The per-block, operator-major evaluation renders one operator over a
 * whole block before moving to the next, so the schedule has to pin
 * - for every operator, where its modulation input comes from (a
 *   same-sample buffer of an already-rendered operator, an msum
 *   timeline value, nothing, or the operator's own feedback pair);
 * - which operator runs its own per-sample loop because its input is
 *   its own previous outputs (the feedback self-loop, lags one and
 *   two);
 * - which operators form one sample-interleaved run because the
 *   algorithm's feedback pair is pushed by one of them and read by
 *   another (algorithms 4 and 6);
 * - which operators must record their sum/delay-register timeline
 *   (only where a later row reads it), and how that value is composed
 *   from the row's C/D bits.
 *
 * Everything the schedule cannot pin degrades to the per-sample walk
 * (`fast` cleared), so a graph the analysis does not understand still
 * renders - through the walk, exactly as the extensions do until they
 * get schedules of their own.
 */
typedef enum {
    SCHED_IN_ZERO = 0,      /* the modulation input is zero */
    SCHED_IN_OBUF,          /* the output buffer of operator `in_src` */
    SCHED_IN_MSUM,          /* the sum/delay timeline of operator `in_src` */
    SCHED_IN_PAIR_AVG,      /* the feedback pair's scaled average
                             * (lags one and two on the op itself) */
    SCHED_IN_PAIR_LAG1      /* the feedback pair's last value (a lag-one
                             * self-loop) */
} slopsix_sched_in;

/* How an operator's sum/delay-register timeline value is composed per
 * sample.  The walk's C/D bits (`msum = (C ? mren : 0) + (D ? signal :
 * 0)`); the `SCHED_M_PREV*` forms read the timeline of the operator one
 * row higher, which is impossible for operator 6 because operator 6's
 * row is the first of the sample. */
typedef enum {
    SCHED_M_DEAD = 0,       /* nothing reads the timeline: skip it */
    SCHED_M_ZERO,           /* C and D clear: the timeline value is 0 */
    SCHED_M_OUT,            /* D only: the operator's signal */
    SCHED_M_PREV,           /* C only: the higher row's timeline value */
    SCHED_M_PREV_OUT        /* C and D: the higher value plus the signal */
} slopsix_sched_m;

typedef struct {
    uint8_t fast;           /* 0: render the algorithm with the walk */
    uint8_t solo;           /* the self-loop operator's index, 0xFF none */
    uint8_t group;          /* the sample-interleaved run's member count,
                             * 0, 2 or 3; the members are the walk
                             * sequence's leading indices */
    uint8_t in_kind[SLOPSIX_OP_COUNT];
    uint8_t in_src[SLOPSIX_OP_COUNT];
    uint8_t msum_mode[SLOPSIX_OP_COUNT];
} slopsix_schedule;

/* One algorithm's connection data. */
typedef struct {
    slopsix_op_conn ops[SLOPSIX_OP_COUNT];  /* index 0 == operator 1 */
    slopsix_schedule schedule;
} slopsix_algorithm;

/* A voice's engine state: the operators, their parameters and their
 * frequencies.  The walk's registers (the pending modulation value and
 * the sum/delay register are the die's shift-register stages, the two
 * feedback registers its feedback pair) survive a sample, and the walk
 * reads one operator of them per row in the hardware's order. */
typedef struct {
    slopsix_operator ops[SLOPSIX_OP_COUNT];
    slopsix_op_params params[SLOPSIX_OP_COUNT];
    slopsix_eg eg[SLOPSIX_OP_COUNT];  /* the operators' envelopes */
    slopsix_pitch_eg pitch_eg;        /* the voice's pitch envelope */
    int32_t modout;                 /* the next row's modulation value:
                                     * the processed row's selector
                                     * produces it and the next row (or
                                     * the next sample's first row)
                                     * consumes it */
    int32_t mren;                   /* the sum/delay register */
    int32_t fren1;                  /* the feedback path's last value */
    int32_t fren2;                  /* the feedback path's value before it */
    uint8_t feedback_level;         /* the OPS's 3-bit feedback field */
    int32_t pitch_offset;           /* the last applied log-frequency
                                     * offset (bend + LFO + pitch EG); the
                                     * render gate skips a rewrite when it
                                     * is unchanged */
    /* The LFO's amplitude modulation reaches the walk as a per-tick,
     * instrument-wide register value (src/slopsix_lfo.c), so it is not
     * cached per voice. */
#if defined(SLOPSIX_DX7S)
    /* The DX7S per-voice LFO (D7S-2 fills and advances it).  The DX7 build
     * compiles this member out, so its struct layout is unchanged.  The
     * type carries the phase, delay accumulator, fade, S&H holder, cached
     * amplitude and the composed pitch-modification word. */
    slopsix_lfo lfo;
#endif
} slopsix_voice_engine;

/* --------------------------------------------------------- context state */

/* The block renderer's scratch, one voice's worth.  Voices are
 * processed one at a time, so it is a context member, not per voice,
 * and it is allocated with the context at creation (the render call
 * does no allocation; plan decision D-15).
 *
 * `opout` holds each operator's rendered samples for the sub-block;
 * `msum` the operators' sum/delay-register timeline values (only the
 * rows some later row reads are written); `level` the operators'
 * pre-composed log amplitudes (Q8, with the compensation and the
 * amplitude-modulation reduction not yet applied); `voice_out` the
 * carrier sums and `bus` the per-sample mix bus for the output
 * stages. */
typedef struct {
    int32_t opout[SLOPSIX_OP_COUNT][SLOPSIX_RENDER_BLOCK];
    int32_t msum[SLOPSIX_OP_COUNT][SLOPSIX_RENDER_BLOCK];
    int16_t level[SLOPSIX_OP_COUNT][SLOPSIX_RENDER_BLOCK];
    int32_t voice_out[SLOPSIX_RENDER_BLOCK];
    int32_t bus[SLOPSIX_RENDER_BLOCK];
} slopsix_block_scratch;

/* The output mix function pointer type.  We use void * for the state
 * argument because the state struct is not yet complete when this
 * declaration is reached; the implementations cast back to the
 * concrete type. */
typedef void (*slopsix_output_mix_fn)(void *state, int32_t bus,
    int16_t *left, int16_t *right);

/* The per-voice bus accumulation.  The DX7 quantizes each voice through
 * the 12-bit compander before summing (the hardware's companded output
 * word); the DX7S's 16-bit output path sums the voice words unchanged, so
 * the DAC sees the operator core's full resolution.  Chosen per
 * personality at init, so the render loop carries no branch. */
typedef void (*slopsix_voice_bus_fn)(const int32_t *voice_out, int32_t *bus,
    unsigned count);

/* The pitch envelope's personality law.  The DX7 plays the firmware's
 * plain envelope: the level word's distance from the neutral value.  The
 * DX7S composes its range, its velocity switch and its rate scaling into
 * that distance (D7S-4), and its law lives under src/dx7s/.  Both entry
 * points are resolved at personality init, like output_mix and
 * voice_bus, so the shared path carries no personality branch and a DX7
 * context in a DX7S build plays the firmware's envelope whatever the
 * shared patch state holds.
 *
 * The configure hook runs at note-on, before the envelope starts; the
 * offset hook reads the envelope's contribution.  Both take the patch
 * and the envelope, so the personality's law sees the parameters it
 * composes and the state it composes them into. */
typedef void (*slopsix_pitch_eg_configure_fn)(const slopsix_patch_state *patch,
    slopsix_pitch_eg *eg, uint8_t note, uint8_t velocity);
typedef int32_t (*slopsix_pitch_eg_offset_fn)(const slopsix_pitch_eg *eg);

typedef struct {
    uint32_t sample_rate;

    /* Pitch. */
    uint16_t pitch_bend;        /* 14-bit, 0x2000 centred */
    uint8_t bend_range;         /* semitones, 0..12 */
    uint8_t bend_step;          /* 0..15 */
    int32_t master_tune_q16;    /* cents, 0 == shipped tuning */

    /* Mix. */
    int32_t pan_q16;            /* 32768 == centre; SLOPSIX_EXTRAS only */
    unsigned master_volume;     /* 0..7 */
    int32_t output_volume_q16;  /* host output gain, Q16; unity by default */
    /* The final conversion's cached gain: the ladder tap times the host's
     * output volume.  Both are applied here, at parameter-change time, so
     * the sample path keeps its single multiply (see slopsix_output.c). */
    int32_t master_gain_q16;

    /* Voice management. */
    bool sustain;
    bool mono;
#if defined(SLOPSIX_DX7S)
    /* DX7S voice management (D7S-3 uses these); the DX7's `mono` above
     * keeps its own Mk1 semantics. */
    uint8_t key_mode;        /* SLOPSIX_KEY_*, poly by default */
    uint8_t unison_detune;   /* 0..7, neutral by default */
    uint8_t lfo_mode;        /* SLOPSIX_LFO_SINGLE/MULTI, single by default */
    bool normalize_output;   /* true: code x16, same level as Mk1 */

    /* The fixed unison groups' bookkeeping (D7S-3).  A group is active
     * from its first key event; `seq` orders the groups for the
     * oldest-first steal and 0 means the group was never used.  The note
     * is the key event the group owns, so a key-off can release every
     * voice of every group that owns it. */
    uint32_t unison_group_seq[SLOPSIX_DX7S_UNISON_GROUPS];
    uint8_t unison_group_note[SLOPSIX_DX7S_UNISON_GROUPS];
    uint8_t unison_group_active[SLOPSIX_DX7S_UNISON_GROUPS];
#endif

    /* Portamento and glissando (the firmware's function parameters and
     * the portamento pedal).  `porta_rate` is the cached rate increment
     * `TABLE_PITCH_EG_RATE[99 - time]`: 0xFF means instantaneous, so the
     * common default is no glide at all.  `porta_last_target` is the
     * firmware's last-note word (its $A4 scratch), which a new
     * polyphonic voice starts from; `porta_bank` is the half-bank
     * toggle PORTA_PROCESS uses. */
    uint8_t porta_time;        /* 0..99 */
    uint8_t porta_rate;        /* 1..255, 0xFF = instantaneous */
    bool porta_full_time;      /* false = fingered, true = full time */
    bool porta_pedal;          /* the portamento pedal's own state */
    bool glissando;
    uint8_t porta_bank;        /* 0 = voices 0..7, 1 = voices 8..15 */
    int32_t porta_last_target;

    /* Mono mode's bookkeeping, following the firmware's own: the active
     * keys are a list of up to sixteen key events (duplicates included,
     * as the firmware's 16-entry buffer holds them), the legato
     * direction selects which remaining key the voice returns to, and
     * M_NOTE_KEY_LAST is the key the next gesture compares against.
     * The sounding voice is always voices[0], keyed and re-pitched in
     * place (research fw VOICE_ADD_MONO / VOICE_REMOVE_MONO). */
    uint8_t mono_keys[SLOPSIX_VOICE_COUNT]; /* the active keys, [0, mono_count) */
    int mono_count;
    uint8_t mono_dir;    /* the legato direction: 1 = up, 0 = down */
    uint8_t mono_last_key;

    /* Controllers.  The value/range/routing triples are the host-side
     * state; the three fields below are the cached arithmetic the
     * firmware keeps in its page zero (M_MOD_*_SCALED_INPUT,
     * M_MOD_AMP_FACTOR, M_MOD_AMP_EG_BIAS_TOTAL).  They are recomputed
     * when a controller value, range or routing changes and at reset,
     * never on the sample path (research fw MOD_PROCESS_INPUT_SOURCES,
     * Phase 4 step 4). */
    uint8_t ctrl_value[SLOPSIX_CTRL_SRC_COUNT];  /* 0..127 */
    uint8_t ctrl_range[SLOPSIX_CTRL_SRC_COUNT];  /* 0..99 */
    unsigned ctrl_assign[SLOPSIX_CTRL_SRC_COUNT];
    uint8_t ctrl_scaled_input[SLOPSIX_CTRL_SRC_COUNT];
    uint8_t mod_amp_factor;     /* the amp-routed sources' clamped sum */
    uint8_t mod_eg_bias_inv;    /* the EG-bias total, inverted polarity */

    /* Voices and the live patch block.  engines[i] renders voices[i];
     * the two arrays are index-paired. */
    slopsix_voice voices[SLOPSIX_VOICE_COUNT];
    slopsix_voice_engine engines[SLOPSIX_VOICE_COUNT];
    slopsix_patch_state patch;
    uint8_t voice_dump[SLOPSIX_VOICE_DUMP_SIZE];
    uint32_t voice_seq;  /* the allocation's sequence counter */

    /* The pitch envelope's global update cadence.  The firmware's toggle
     * is global, so the cadence belongs here rather than in a voice. */
    slopsix_pitch_eg_cadence pitch_cadence;

    /* The LFO: one per instrument, advanced once per OCF tick on the
     * render path (research E-04/L-01).  The activated parameters are
     * recomputed at each patch load; the runtime state runs
     * continuously. */
    slopsix_lfo lfo;
    slopsix_lfo_cadence lfo_cadence;

    /* The analog tail: the board's reconstruction filter after the DAC
     * step (the DX7's own output stage; the algorithm page §8.1).  Its
     * design follows the host's rate and its state runs continuously. */
    slopsix_filter filter;

    /* The block renderer's scratch (one voice's worth, above). */
    slopsix_block_scratch block_scratch;

    /* The output mix function: the personality's own output stage, chosen
     * at context creation.  The DX7 uses slopsix_output_mix; the DX7S uses
     * slopsix_dx7s_output_mix.  The pointer is set once, so
     * slopsix_render_span calls through it without a branch in the sample
     * loop. */
    slopsix_output_mix_fn output_mix;

    /* The per-voice bus stage: the DX7 compander or the DX7S raw sum.
     * Set once at personality init for the same reason. */
    slopsix_voice_bus_fn voice_bus;

    /* The pitch envelope's personality law: the note-on composition and
     * the level's contribution to the log-frequency word.  The DX7 plays
     * the firmware's plain envelope; the DX7S composes its range, its
     * velocity switch and its rate scaling into it (D7S-4).  Set once at
     * personality init, for the same reason. */
    slopsix_pitch_eg_configure_fn pitch_eg_configure;
    slopsix_pitch_eg_offset_fn pitch_eg_offset;
} slopsix_context_state;

/* ------------------------------------------------------- the personality */

/*
 * The context implementation.
 *
 * The context is opaque to the host, and it carries the selected engine
 * and that engine's own state.  The render entry point is resolved at
 * creation and stored here, so the render call is one indirect call per
 * buffer and neither the sample loop nor the voice loop carries a
 * personality branch (plan decision D-18).
 *
 * The DX7's state is the first member, so the DX7 path's own code can
 * reach it without a branch; the accessor macros below do that.
 */
typedef struct {
    slopsix_engine engine;
    uint32_t sample_rate;
    void (*process)(slopsix_context ctx, int16_t *stereo_out, size_t frames);

    slopsix_context_state dx7;
} slopsix_context_impl;

/* The context handle is the context implementation; the public API uses
 * void *. */
#define SLOPSIX_IMPL(ctx) ((slopsix_context_impl *)(ctx))

/* The DX7 path's state inside a context.  Only the DX7 path uses it. */
#define SLOPSIX_CONTEXT(ctx) (&SLOPSIX_IMPL(ctx)->dx7)

/* The DX7 personality's own entry points.  They live in the DX7's own
 * translation units; the shared API glue calls them through the
 * personality's dispatch. */
void slopsix_dx7_init(slopsix_context_state *state, uint32_t sample_rate);
void slopsix_dx7_reset(slopsix_context_state *state);
void slopsix_dx7_set_sample_rate(slopsix_context_state *state,
    uint32_t sample_rate);
void slopsix_dx7_process(slopsix_context ctx, int16_t *stereo_out,
    size_t frames);

/* The Mk1's note events (src/slopsix.c).  The DX7S Poly and Mono modes
 * delegate here, so the personality's own file owns only the unison
 * deltas. */
void slopsix_dx7_note_on(slopsix_context_state *state, uint8_t note,
    uint8_t velocity);
void slopsix_dx7_note_off(slopsix_context_state *state, uint8_t note);
void slopsix_dx7_all_notes_off(slopsix_context_state *state);

#if SLOPSIX_HAS_DX7S
/* The DX7S personality's own entry points.  They live under src/dx7s/ and
 * are compiled in only when SLOPSIX_DX7S is defined; the state is the
 * shared context state (the DX7S is a superset of the DX7 path). */
void slopsix_dx7s_init(slopsix_context_state *state, uint32_t sample_rate);
void slopsix_dx7s_reset(slopsix_context_state *state);
void slopsix_dx7s_set_sample_rate(slopsix_context_state *state,
    uint32_t sample_rate);
void slopsix_dx7s_process(slopsix_context ctx, int16_t *stereo_out,
    size_t frames);

/* The per-voice LFO engine (src/dx7s/slopsix_lfo.c): patch activation
 * fills every voice's parameters, the tick advances all sixteen voice
 * LFOs on the shared OCF gate, and note-on resets the keyed voice's own
 * delay/fade/phase. */
void slopsix_dx7s_lfo_patch_activate(slopsix_context_state *state,
    const uint8_t *dump);
void slopsix_dx7s_lfo_tick(slopsix_context_state *state);
void slopsix_dx7s_lfo_note_on(slopsix_context_state *state,
    slopsix_voice_engine *engine);

/* The DX7S output stage (src/dx7s/slopsix_output.c): the wide voice
   * accumulator, the 15-bit DA word (the sum divided by sixteen, the
   * only saturation the mix meets - rails it never reaches below the
   * sixteen-voice window), the analog tail filter, and the int16
   * delivery of the DAC code.  Unlike the Mk1, the DX7S has no
   * compander in the voice path: the voices sum linearly in the
   * accumulator, and the transmitted word is their sum divided by
   * sixteen (SLOPSIX_DX7S_DA_SHIFT).  The delivery scale is the DAC
   * code times eight by default (SLOPSIX_DX7S_OUTPUT_SCALE_SHIFT: -6 dB
   * vs the Mk1, one voice at 8184); the normalize_output flag switches
   * it to times sixteen (SLOPSIX_DX7S_NORMALIZED_SCALE_SHIFT: same
   * level as the Mk1, one voice at 16368), though the analog ceiling
   * compresses the hottest stack past the knee at 24564 near the rail.
   * The master
   * volume is the shared hardware
  * ladder (slopsix_master_gain_q16): the DX7S has the same resistor
  * string as the Mk1. */
int32_t slopsix_dx7s_dac_step(int32_t bus);
int16_t slopsix_dx7s_output_scale(int32_t code_q12, int32_t gain_q16,
    unsigned shift);
/* The DX7S per-voice bus stage: add the voice words into the wide
 * accumulator.  No compander, no per-voice output stage, no master
 * rail below the DA word. */
void slopsix_dx7s_voice_bus(const int32_t *voice_out, int32_t *bus,
    unsigned count);
/* The DX7S analog tail (src/dx7s/slopsix_filter.c): the personality's own
 * reconstruction prototype, designed through the shared filter core.  The
 * analog node's value enters the core directly (no downshift: the core
 * scales the signal to Q12 internally, and the node's sixteen-voice
 * window fits the Q12 domain with room for the low-pass's overshoot).
 *
 * The DX7S tail is a generated descriptor table (a 21 kHz Butterworth
 * pair and, first in the cascade, the aperture shelf); it is compiled only
 * into the DX7S personality. */
#ifdef SLOPSIX_DX7S
extern const int32_t
    slopsix_dx7s_filter_desc[SLOPSIX_FILTER_SECTIONS][4];
#endif
void slopsix_dx7s_filter_init(slopsix_filter *filter, uint32_t sample_rate);
void slopsix_dx7s_output_mix(void *state, int32_t bus,
    int16_t *left, int16_t *right);

/* The DX7S's pitch-envelope law (src/dx7s/slopsix_pitch_eg.c): the
 * note-on composition of the range, the velocity switch and the rate
 * scaling (plan D7S-4), and the composed contribution of the envelope
 * to the voice's log-frequency word. */
void slopsix_dx7s_pitch_eg_configure(const slopsix_patch_state *patch,
    slopsix_pitch_eg *eg, uint8_t note, uint8_t velocity);
int32_t slopsix_dx7s_pitch_eg_offset(const slopsix_pitch_eg *eg);

/* The DX7S's voice management (src/dx7s/slopsix_voice.c, D7S-3).  Poly
 * and Mono delegate to the Mk1's paths; the two unison modes use the
 * fixed groups.  The detune refresh recomposes every grouped voice's
 * pitch offset when the detune parameter changes, and a key-mode change
 * clears the group bookkeeping (the sounding voices are left alone, as
 * the Mk1's mode switch leaves its EGS voices alone). */
void slopsix_dx7s_note_on(slopsix_context_state *state, uint8_t note,
    uint8_t velocity);
void slopsix_dx7s_note_off(slopsix_context_state *state, uint8_t note);
void slopsix_dx7s_all_notes_off(slopsix_context_state *state);
void slopsix_dx7s_apply_detune(slopsix_context_state *state);
void slopsix_dx7s_clear_groups(slopsix_context_state *state);
#endif

/* The shared render-span helpers (src/slopsix_voice.c): the DX7S process
 * loop reuses the same cadence arithmetic and voice mix as the DX7 loop. */
uint32_t slopsix_cadence_threshold(uint32_t sample_rate);
uint32_t slopsix_cadence_distance(const slopsix_cadence *cad,
    uint32_t threshold);
void slopsix_cadence_skip(slopsix_cadence *cad, size_t samples);
int slopsix_cadence_step(slopsix_cadence *cad, uint32_t threshold);
void slopsix_render_span(slopsix_context_state *state,
    const slopsix_algorithm *alg, int16_t *stereo_out, size_t start,
    size_t count);

/* ------------------------------------------------------ internal API */

/* The operator core.  These run for every operator of every voice on every
 * sample, so their bodies live here and are inlined into the render path's
 * translation units rather than being called across translation units.
 *
 * The pipeline follows the hardware's (research O-02, O-04):
 *
 *   sine phase (12 bits)  ->  log-sine (Q10, negated log2 of |sin|)
 *   log amplitude (Q10)   ->  14-bit log word (Q10)
 *   log word              ->  14-bit linear sample (the signal exp)
 *
 * The modulation input is a phase addition: the modulating operator's
 * linear output is added to the modulated operator's phase before the
 * sine lookup (the patent's sin{K.wt + f(w.mt)} form).
 */

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
static inline int32_t slopsix_logsin(uint32_t sine_phase)
{
    uint32_t index = sine_phase & SLOPSIX_SINE_PHASE_MASK;
    uint32_t quarter = index & SLOPSIX_SINE_INDEX_MASK;

    if (index & SLOPSIX_SINE_QUARTER_BIT) {
        /* The second quarter of the peak: the index is complemented. */
        quarter = SLOPSIX_SINE_INDEX_MAX - quarter;
    }

    return (int32_t)slopsix_logsin_quarter[quarter];
}

/*
 * The exponential recipe.
 *
 * The 14-bit log word (Q10: four integer octaves and a ten-bit fraction)
 * is converted to a linear value with the hardware's recipe:
 *
 *   value = round(2^frac * 2048) << int >> shift
 *
 * The ROM's fraction table is the generated exp table; the integer part
 * is the chip's four-bit field, so a word above the field's range
 * saturates at the top and a word below zero (below the exponent range)
 * is silence.
 *
 * The shift is the domain's own: the operator core's 14-bit sample uses
 * 13 and the frequency path's 22-bit phase increment uses 5.
 */
static inline uint32_t slopsix_exp_value(int32_t log_q10, unsigned shift)
{
    int32_t int_part;
    int32_t frac;

    if (log_q10 < 0) {
        /* Below the exponent range: silence. */
        return 0u;
    }

    int_part = log_q10 >> 10;
    frac = log_q10 & 1023;

    if (int_part > 15) {
        /* The integer field is four bits: the value saturates. */
        int_part = 15;
        frac = 1023;
    }

    return ((uint32_t)slopsix_exp_q11[frac] << (uint32_t)int_part) >> shift;
}

/* The signal exponent: the operator core's 14-bit sample. */
static inline int32_t slopsix_signal_exp(int32_t log_q10)
{
    return (int32_t)slopsix_exp_value(log_q10, 13u);
}

/*
 * The feedback path.
 *
 * The hardware averages the last two stored values of the feedback pair
 * before feeding them back (the die's two feedback registers and the
 * averaging adder), and the result is divided by a power of two (the
 * 0..6-bit feedback shifter).  The level-to-shift mapping is 7 - level,
 * which a bit-synchronous OPS model corroborates (research O-06); the
 * path's own value is pushed by the operator whose row carries the
 * feedback-store control bit (research A-01 - in most algorithms that is
 * the reading operator itself).
 */
static inline int32_t slopsix_feedback_scaled(int32_t fren1, int32_t fren2,
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

    /* The average of the path's last two stored values. */
    value = (fren1 + fren2) >> 1;

    /* The feedback shifter's division. */
    shift = (int32_t)SLOPSIX_FB_SHIFT(feedback_level);
    value >>= shift;

    return value;
}

/*
 * One operator's per-sample stage, with the phase held in registers:
 * the same arithmetic as slopsix_op_render, but with the accumulator
 * and its increment as values so the block renderer's loops can keep
 * them resident across the block.  `phase_out` receives the updated
 * accumulator.
 */
static inline void slopsix_op_stage(uint32_t phase, uint32_t increment,
    int32_t log_amp_q10, int32_t mod_in, uint32_t *phase_out, int32_t *out)
{
    uint32_t sine_phase;
    int32_t log_value;
    int32_t sample;

    /* The modulation input - the sum of the operator's input buses, or
     * the feedback path's value - is added to the phase for the lookup
     * only: it is not part of the accumulator's state.  It is not
     * clamped: the hardware adds the operator's linear output (or the
     * modulator sum) as it is, and the phase addition wraps. */
    {
        uint32_t lookup = phase +
            (uint32_t)(mod_in * (1 << SLOPSIX_MOD_DEVIATION_SHIFT));

        sine_phase = (lookup >> SLOPSIX_SINE_PHASE_SHIFT) &
                     SLOPSIX_SINE_PHASE_MASK;
    }

    /* The log word: the operator's pre-shifted log amplitude (Q10) minus
     * the log-sine's negated log2 value. */
    log_value = log_amp_q10 - slopsix_logsin(sine_phase);

    sample = slopsix_signal_exp(log_value);

    if (sine_phase & SLOPSIX_SINE_HALF_BIT) {
        /* The negative half of the wave inverts the output. */
        sample = -sample;
    }

    *phase_out = (phase + increment) & SLOPSIX_PHASE_MASK;

    *out = sample;
}

/*
 * Render one operator.
 *
 * The phase advances by the operator's increment; the modulation input
 * (and the operator's own feedback) are added to the phase before the
 * sine lookup.  The output is the 14-bit linear sample.
 *
 * The log amplitude is passed in rather than read from the parameters,
 * because the envelope and the carrier-count compensation compose it
 * on every sample.
 */
static inline void slopsix_op_render(slopsix_operator *op,
    int32_t log_amp_q10, int32_t mod_in, int32_t *out)
{
    uint32_t acc = op->phase;
    uint32_t next;

    slopsix_op_stage(acc, op->increment, log_amp_q10, mod_in, &next, out);
    op->phase = next;
}

/* ------------------------------------------------- the render composites */

/*
 * The carrier-count compensation, in the Q8 domain: a negative value
 * that the carrier's log amplitude carries (research A-05).  The code
 * is static per algorithm, so it is composed once when the algorithm's
 * table is decoded and read from there by the render path.
 */
static inline int32_t slopsix_com_q8(uint8_t com)
{
    if (com > 5u) {
        com = 5u;
    }
    return -((int32_t)slopsix_com_log[com] << 4);
}

/*
 * The amplitude modulation's reduction, in the Q8 domain: the amount by
 * which the LFO lowers an operator's level.  The EGS form is an
 * attenuation addition, `a += amp_mod << ams` (algorithm page §4.5); in
 * the engine's gain direction the same quantity is this subtraction, the
 * 12-bit attenuation unit and the Q8 unit being the same 1/256-octave
 * step with opposite sign.  The reduction never raises the level; the
 * caller's clamp at the 12-bit envelope floor (silence) bounds it.
 * `amp_mod_register` is the per-tick value of the EGS's amp-mod register
 * (src/slopsix_lfo.c); the shift is the whole depth law, and sensitivity
 * 0 disables the path (an operator without sensitivity is never
 * modulated).  The register changes only on the LFO's ticks, so the
 * reduction is composed at note-on and on every tick and cached in the
 * voice's operator parameters; the render path only reads the cached
 * value.
 *
 * The sensitivity's width is the personality's.  The Mk1's voice data
 * field is 2 bits (0..3), and at 3 the 8-bit register spans 2040 of the
 * 12-bit range, so the reduction never reaches the floor by itself.  The
 * DX7S's additional voice data widens it to 3 bits (0..7; algorithm page
 * §4.5.1); 0..3 keep the Mk1's meaning and 4..7 extend the same shift
 * law (hypothesis-grade, OQ-DX7S-09).  At 4 the full register spans
 * 4080, one step short of the 12-bit envelope floor (0xFFF, the EGS's
 * own clamp), and from 5 up the shift alone passes it, so the DX7S build
 * saturates the reduction there.  A DX7 context never holds a
 * value above 3 (the dump field is masked and the setter is DX7S-only),
 * so the saturation never acts on its path; the DX7-only build compiles
 * it out.
 *
 * The width is selected per build rather than per engine, so in a DX7S
 * build the DX7 personality shares this wide helper and its path carries
 * the wide clamp and floor.  That is output-neutral only because of the
 * DX7 value cap above.  Splitting the helper per engine would put the
 * personality check on the amplitude-modulation reduction path, which the
 * sample loop evaluates for every operator of every voice, so the
 * build-time choice is kept and the cap is what keeps the shared helper
 * from changing the DX7 output.
 */
#if defined(SLOPSIX_DX7S)
#define SLOPSIX_AMS_SENS_MAX 7u
#else
#define SLOPSIX_AMS_SENS_MAX 3u
#endif

/* The 12-bit envelope attenuation's floor, in Q8 units (§4.5). */
#define SLOPSIX_AMS_REDUCTION_FLOOR 0xFFF

static inline int32_t slopsix_ams_reduction_q8(uint8_t ams_sens,
    uint8_t amp_mod_register)
{
    int32_t reduction;

    if (ams_sens == 0u) {
        return 0;
    }
    if (ams_sens > SLOPSIX_AMS_SENS_MAX) {
        ams_sens = SLOPSIX_AMS_SENS_MAX;
    }
    reduction = (int32_t)amp_mod_register << ams_sens;
#if defined(SLOPSIX_DX7S)
    if (reduction > SLOPSIX_AMS_REDUCTION_FLOOR) {
        reduction = SLOPSIX_AMS_REDUCTION_FLOOR;
    }
#endif
    return reduction;
}

/* Floor division by a power of two, defined for negative values.  C leaves
 * the result of a right shift on a negative integer implementation-defined,
 * so the conversions that can see a signed word use this instead. */
static inline int32_t slopsix_floor_div(int32_t value, unsigned shift)
{
    if (value >= 0) {
        return value >> shift;
    }
    return -(((-value) + (int32_t)((1u << shift) - 1u)) >> shift);
}

/* Floor division by a power of two, for the 64-bit products the output
 * stage's stages work with (the filtered value in Q12 times the pan's
 * Q15 amount). */
static inline int64_t slopsix_floor_div64(int64_t value, unsigned shift)
{
    if (value >= 0) {
        return value >> shift;
    }
    return -(((-value) + ((int64_t)((1u << shift) - 1u))) >> shift);
}

/* ----------------------------------------------------- output (Phase 5) */

/* The output chain mirrors the hardware's (algorithm page §8, plan
 * decision D-5): the per-voice carrier sum is quantized by the compander
 * into a 12-bit mantissa plus a 3-bit shift, the quantized voices sum on
 * a wide bus, the sum passes the master DAC's 12-bit code step with
 * saturation, the analog tail's reconstruction filter follows (the
 * board's low-pass after the DAC and the sample-and-hold circuits, §8.1),
 * the optional pan splits the mono result (SLOPSIX_EXTRAS), and the host's
 * output volume composed with the 3-bit master volume scales the DAC's
 * reference before the int16 conversion.
 * The stages are distinct functions with their own fixed widths, so a
 * later extension can replace the quantization or the conversion without
 * touching the mixer (plan decision D-15).
 *
 * The compander runs per voice, so it is inline here like the operator
 * core; the once-per-sample stages live in src/slopsix_output.c and the
 * filter in src/slopsix_filter.c. */

/* One transmitted compander word: the 12-bit two's-complement datum the
 * DAC converts, plus the shift whose analog scale the board applies. */
typedef struct {
    int16_t mantissa;   /* the 12-bit datum, -2048..2047 */
    uint8_t shift;      /* the SF shift code, 0..3 */
} slopsix_compander_word;

/* Quantize one voice's carrier sum into the compander word.
 *
 * The word's value (mantissa << shift) is the input truncated to the
 * mantissa's 12-bit window; the dropped low bits are the quantization
 * error, and the floor direction for negative inputs is towards minus
 * infinity.  `sample` is a voice word as the graph walk produces it; the
 * mantissa's clamp covers anything beyond a single voice's reach. */
static inline void slopsix_compander_quantize(int32_t sample,
    slopsix_compander_word *word)
{
    uint32_t magnitude = sample < 0 ? 0u - (uint32_t)sample : (uint32_t)sample;
    unsigned shift;
    int32_t mantissa;

    if (magnitude < SLOPSIX_COMPANDER_THRESHOLD_1) {
        shift = 0;
    } else if (magnitude < SLOPSIX_COMPANDER_THRESHOLD_2) {
        shift = 1;
    } else if (magnitude < SLOPSIX_COMPANDER_THRESHOLD_3) {
        shift = 2;
    } else {
        shift = SLOPSIX_COMPANDER_SHIFT_MAX;
    }

    mantissa = slopsix_floor_div(sample, shift);

    if (mantissa > SLOPSIX_COMPANDER_MANTISSA_MAX) {
        mantissa = SLOPSIX_COMPANDER_MANTISSA_MAX;
    } else if (mantissa < SLOPSIX_COMPANDER_MANTISSA_MIN) {
        mantissa = SLOPSIX_COMPANDER_MANTISSA_MIN;
    }

    word->mantissa = (int16_t)mantissa;
    word->shift = (uint8_t)shift;
}

/* The quantized word's value, in the bus's unit (the truncated input).
 * The scaling is a multiplication: the mantissa is signed and the shift
 * is a variable, so no constant-shift trick applies. */
static inline int32_t slopsix_compander_value(const slopsix_compander_word *word)
{
    return (int32_t)word->mantissa * (1 << word->shift);
}

/* The master DAC grid: the mixed bus reduced to the 12-bit code grid.
 * The per-voice compander owns the code window, so this stage does not
 * saturate (the constants above carry the model's grade). */
int32_t slopsix_dac_step(int32_t bus);

/* The DX7 per-voice bus stage: quantize every voice through the compander
 * and add its value to the bus.  One call per voice per render block. */
void slopsix_voice_bus_companded(const int32_t *voice_out, int32_t *bus,
    unsigned count);

/* The master volume's gain: the hardware's 8-level DAC reference ladder,
 * level 0 the quietest tap, level 7 the top tap, as a Q16 fraction of
 * the top tap. */
int32_t slopsix_master_gain_q16(unsigned level);

/* The output stage's headroom, in bits: the model's reference point
 * (the 12-bit DAC code window, one full-scale voice) sits this far below
 * int16 full scale.  One bit is 6.02 dB.
 *
 * The reference is the engine's choice, not a hardware fact: the DAC's
 * absolute span and the board's analog stage are deliberately out of the
 * model (research OQ-24, OU-04), so only the relative levels are pinned.
 * One bit matches the VDX7 reference renderer's convention and leaves
 * 6 dB for a host gain or a future effects chain.  The int16 conversion
 * is the chain's only saturation now; it is the analog stage's rail, and
 * the headroom sets where a polyphonic sum reaches it.
 *
 * Changing this changes every rendered level, so the output stage's
 * absolute-level checks in the harness move with it. */
#define SLOPSIX_OUTPUT_HEADROOM_BITS 1u

/* The final conversion: the analog stage's output times a Q16 gain,
 * rounded and mapped onto int16.  The value is the filter's output in
 * Q12 of a DAC code; the conversion saturates at the int16 rails, which
 * the analog stage's overshoot can reach. */
int16_t slopsix_output_scale(int32_t code_q12, int32_t gain_q16);

#if defined(SLOPSIX_EXTRAS)
/* The extras pan law: split the mono signal across the two channels.
 * Unity gain at centre (each channel carries the mono signal exactly),
 * fading linearly to silence on the far half of the travel; the law's
 * shape beyond that is an open extras question (research X-06) - the
 * centre's faithfulness is what is pinned (plan decision D-21). */
void slopsix_pan_split(int32_t pan_q16, int32_t mono, int32_t *left,
                       int32_t *right);
#endif

/* The once-per-sample composite the render call uses: the DAC step, the
 * analog tail's filter, the compile-time-selected pan and the master
 * conversion, from the mixed bus into the two int16 channels.  The
 * personality's stage is chosen at context creation (the function pointer
 * above), so the sample loop carries no branch per personality (plan
 * decision D-18's rule on the output path).  The filter is stateful, so
 * the state is not const.  The state parameter is void * to match the
 * function pointer typedef. */
void slopsix_output_mix(void *state, int32_t bus,
                         int16_t *left, int16_t *right);

/* The pitch bend (Phase 4). */

/* The EGS's pitch-modification word: 16 bits in units of 1/16384 of an
 * octave, that is, two fractional bits below the EGS's 1024-per-octave
 * frequency datum.  The engine's log-frequency word is the firmware's
 * 4096-per-octave word (plan decision D-12), so the pitch mod enters it
 * shifted right by two: the two units denote the same pitch, and the
 * shift is the conversion between the chips' domains. */
#define SLOPSIX_PITCH_MOD_UNITS_PER_OCTAVE 16384
#define SLOPSIX_PITCH_MOD_TO_LOG_SHIFT 2

/* The bend's pitch-modification word, in the EGS's 1/16384-octave units.
 * The bend range is 0..12 semitones and the step is 0..15 (0 =
 * continuous); a non-zero step ignores the range, as on the hardware. */
int32_t slopsix_bend_pitch_mod(uint16_t bend14, uint8_t range, uint8_t step);

/* The bend's contribution to the voice's log-frequency word, in the
 * engine's 4096-per-octave units. */
int32_t slopsix_bend_log_offset(uint16_t bend14, uint8_t range, uint8_t step);

/* The bend's contribution for a context: the bend, its range and its step
 * are global, as on the hardware. */
int32_t slopsix_bend_context_offset(const slopsix_context_state *state);

/* ----------------------------------------- the LFO and the EGS registers */

/* One LFO update: the hardware's per-tick routine (the algorithm page
 * §6.6), advancing the delay/fade pair, the 16-bit phase accumulator and
 * the S&H holder, computing the wave amplitude, and composing both
 * registers' values - the amp-mod register (§4.5) and the LFO's own share
 * of the pitch-modification word (§6.5).  The bend's part of the word is
 * added here from the context's current bend, so the register's word is
 * always current.
 *
 * The caller runs this on the samples where the cadence gate fires, once
 * per render call; the state itself carries no cadence. */
void slopsix_lfo_tick(slopsix_context_state *state);

/* The same per-tick work on one LFO state: the DX7S personality's
 * per-voice LFO reuses it with the voice's own state instead of the
 * context's. */
void slopsix_lfo_tick_one(slopsix_context_state *state, slopsix_lfo *lfo);

/* The waveform recipe as a pure phase->amplitude step (the DX7S's
 * per-voice LFO calls it as well; the recipes are not duplicated). */
void slopsix_lfo_wave_step(slopsix_lfo *lfo, unsigned sh_flag);

/* The LFO's life cycle: the shared cadence gate (above) and the note-on
 * hook - the firmware's VOICE_ADD side, which clears the delay
 * accumulator and the fade factor when the patch's delay is not 0, and
 * seeds the phase accumulator to 0x7FFF when the patch's LFO SYNC is set.
 * The LFO state is global, so the hook runs on every note-on, as the
 * firmware's voice-add paths do. */
void slopsix_lfo_note_on(slopsix_context_state *state);

/* The note-on reset on one LFO state (the DX7S per-voice hook). */
void slopsix_lfo_note_on_one(slopsix_context_state *state, slopsix_lfo *lfo);

/* The patch activation's LFO parse: the dump's bytes 137..143 through the
 * firmware's own activation scaling - the two closed laws for the
 * increments, the shared value scale's high bytes for the depths, the
 * PMS table for the sensitivity and the raw byte for the waveform (the
 * algorithm page §6.6). */
void slopsix_lfo_patch_activate(slopsix_context_state *state,
    const uint8_t *dump);

/* The parameter half of the patch activation, on one LFO state. */
void slopsix_lfo_activate(slopsix_lfo *lfo, const uint8_t *dump);

/* The two activation laws, exposed for the harness's checks: the speed
 * parameter's increment (speed 0 seeds the accumulator with 11) and the
 * delay parameter's counter increment. */
uint16_t slopsix_lfo_speed_increment(uint8_t speed);
uint16_t slopsix_lfo_delay_increment(uint8_t delay);

/* The pitch-modification register's word for a context: the LFO's
 * per-tick share plus the current bend, wrapped at 16 bits (the
 * firmware's ADD; the wrap is the word's only saturation).  The bend
 * setters recompute it so a bend change is audible without waiting for
 * the next tick. */
int16_t slopsix_mod_pitch_word(const slopsix_context_state *state);
int16_t slopsix_mod_pitch_word_one(const slopsix_context_state *state,
    const slopsix_lfo *lfo);

/* The controller arithmetic: the four input sources' scaled inputs, the
 * amplitude-modulation factor and the EG-bias total, recomputed whenever
 * a controller value, range or routing changes and at reset - never on
 * the sample path (research fw MOD_PROCESS_INPUT_SOURCES, ROM $E208).
 *
 * The scaled input keeps the MUL pair's high byte: the analog input
 * (0..127) times the range's scaled high byte, (660*range) >> 8 from the
 * shared value scale.  The amp-routed sources' scaled inputs sum, clamped
 * at $FF, into the amplitude-modulation factor.  The EG-bias total is
 * inverted polarity: the EG-routed ranges' scaled values first sum (clamped
 * at $FF) and are complemented once, then the EG-routed inputs' clamped sum
 * is added (clamped at $FF again) and the total is complemented for the
 * amp-mod register's composition.  With 0..127 inputs this total is the
 * range's sum minus the inputs' sum, so nothing routed leaves it at 0. */
void slopsix_mod_process_input_sources(slopsix_context_state *state);

/* The pitch-modification word's wave term: the factor is the pitch-routed
 * sources' scaled inputs added to the LFO's own depth term (clamped at
 * $FF); the term is that factor times one byte of the sensitivity-scaled
 * amplitude.  The tick recomputes it with the fresh amplitude; the
 * controller setters recompose it with the last tick's amplitude, so a
 * controller change is audible without waiting for the next tick. */
void slopsix_mod_pitch_term_update(slopsix_context_state *state);
void slopsix_mod_pitch_term_update_one(slopsix_context_state *state,
    slopsix_lfo *lfo);

/* The composed register word's contribution to the voice's log-frequency
 * word, in the engine's 4096-per-octave units (floor division by the
 * domain conversion's shift, as the bend's alone does). */
int32_t slopsix_mod_pitch_offset(const slopsix_context_state *state);

/* One pitch-EG update (the firmware's PITCH_EG_PROCESS, per voice).
 *
 * The caller runs this on the samples where the context's cadence gate
 * fires, once per sounding voice; the envelope itself carries no cadence,
 * so the step and the gate cannot double-step an envelope. */
void slopsix_pitch_eg_update(slopsix_pitch_eg *eg);

/* The current level as a contribution to the log-frequency word, in the
 * engine's 4096-per-octave units (neutral 0, about -4..+3.97 octaves).
 *
 * This is the Mk1's law: the level's distance from the neutral value.
 * The personality's own law is the context's pitch_eg_offset hook. */
int32_t slopsix_pitch_eg_offset(const slopsix_pitch_eg *eg);

/* The pitch envelope's life cycle. */
void slopsix_pitch_eg_init(slopsix_pitch_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4]);
void slopsix_pitch_eg_note_on(slopsix_pitch_eg *eg);
void slopsix_pitch_eg_note_off(slopsix_pitch_eg *eg);

/* The DX7's pitch-EG composition: none.  The Mk1 plays its envelope as
 * the firmware plays it, so this is the no-op the context's
 * pitch_eg_configure hook points at. */
void slopsix_pitch_eg_configure(const slopsix_patch_state *patch,
    slopsix_pitch_eg *eg, uint8_t note, uint8_t velocity);

/* The frequency path.  `pitch_offset` is the voice's total log-frequency
 * offset (the pitch bend and the pitch EG), which the EGS applies to a
 * ratio-mode operator only.
 *
 * The voice's word - the note, the pitch offset, the floor clamp and the
 * master tune - is shared by every ratio-mode operator, so the render path
 * composes it once per voice (slopsix_voice_word) and then applies each
 * operator's own ratio and detune (slopsix_op_freq_from_voice).  The
 * note-taking form below does both steps for a single operator and is the
 * convenience entry for direct callers. */
int32_t slopsix_voice_word(uint8_t note, int32_t master_tune_q16,
                           int32_t pitch_offset);
/* The same composition from an already-composed base word instead of a
 * note.  Portamento supplies its current (or glissando) word here, since
 * the glide runs in the same log-frequency domain as the note. */
int32_t slopsix_voice_word_from_base(int32_t base_word,
                                     int32_t master_tune_q16,
                                     int32_t pitch_offset);
int32_t slopsix_op_freq_from_voice(const slopsix_op_params *params,
                                   int32_t voice_word);
int32_t slopsix_op_freq_word(uint8_t note, const slopsix_op_params *params,
                             int32_t master_tune_q16, int32_t pitch_offset);
uint32_t slopsix_increment_from_word(int32_t word, uint32_t sample_rate);

/* The operator graph. */
const slopsix_algorithm *slopsix_algorithm_get(unsigned index);
uint8_t slopsix_algorithm_feedback_operator(unsigned index);
void slopsix_voice_render_sample(slopsix_voice_engine *voice,
                                  const slopsix_algorithm *alg,
                                  int32_t *voice_out);

/* The block renderer: the per-sample walk's evaluation order reorganized
 * per algorithm schedule - one operator over the whole block before the
 * next, envelopes filled per block, every routing decision taken once
 * per operator and block instead of per sample.  Bit-identical to
 * `count` slopsix_voice_render_sample calls on the same voice (and it
 * falls back to calling them, per sample, for the algorithms whose
 * schedule the analysis could not pin).  `count` is the sub-block's
 * length; it may exceed the block size, in which case the call renders
 * consecutive blocks.  `scratch` is the caller's per-context workspace;
 * the carrier sums land in `voice_out`.
 */
void slopsix_voice_render_block(slopsix_voice_engine *voice,
                                 const slopsix_algorithm *alg,
                                 slopsix_block_scratch *scratch,
                                 int32_t *voice_out, unsigned count);

/* The voice path (Phase 5).  The note-on fills one voice's engine from
 * the live patch, keying it as the firmware's VOICE_ADD does (envelopes
 * out of silence, the pitch EG reset to its initial level, the phases
 * reset when the patch's OSC KEY SYNC bit is set); the release does the
 * VOICE_REMOVE side; the frequency rewrite composes the portamento (or
 * glissando) word, the master tune and the voice's current pitch offset
 * into every operator's phase increment. */
void slopsix_voice_engine_note_on(slopsix_context_state *state,
    slopsix_voice *slot, slopsix_voice_engine *engine);
void slopsix_voice_engine_release(slopsix_voice_engine *engine);
/* The patch activation's VOICE_RESET_EGS side: silence every operator's
 * envelope without touching the voice's key bookkeeping. */
void slopsix_voice_engine_silence(slopsix_voice_engine *engine);
void slopsix_voice_engine_update_frequency(slopsix_context_state *state,
    const slopsix_voice *slot, slopsix_voice_engine *engine);
void slopsix_voice_engine_update_ams(slopsix_context_state *state,
    slopsix_voice_engine *engine);
void slopsix_refresh_pitch(slopsix_context_state *state);

/* ------------------------------------------------------- portamento (P-06) */

/* The pitch-EG rate table's entry, shared with the portamento rate law:
 * portamento time 0..99 indexes it as `99 - time`. */
uint8_t slopsix_pitch_eg_rate_value(uint8_t rate);

/* One voice's portamento step, in the firmware's own words.  The step is
 * proportional to the distance's high byte (its `>> 2` quarters plus
 * one) times the cached rate; it clamps at the target.  When glissando
 * is on, the glissando word walks toward the new portamento word in
 * semitone-quantized steps and is the returned output word; otherwise
 * the portamento word itself is returned.  `current` and `gliss` are
 * updated in place. */
int32_t slopsix_porta_step_word(int32_t *current, int32_t target,
                                int32_t *gliss, uint8_t rate,
                                bool glissando);

/* The per-tick PORTA_PROCESS over one half of the voice bank.  It steps
 * the given bank (0 = voices 0..7, 1 = voices 8..15) and returns a bit
 * mask of the voices whose audible word changed, so the caller rewrites
 * exactly those phase increments. */
uint16_t slopsix_porta_process(slopsix_context_state *state, unsigned bank);

/* ------------------------------------------------- the envelope (Phase 3) */

/* The level curve: the hardware's 0..99 level parameter mapped onto the
 * engine's 0..127 microstep gain.  The law is closed: below 20 the gain
 * follows the hardware's own curve, from 20 upwards it is linear. */
int32_t slopsix_level_curve(uint8_t param);

/* The keyboard level scaling builder: it fills the 43 per-group offsets
 * of one operator.  The offsets are signed gains in microsteps: a
 * positive offset raises the level, a negative one lowers it. */
void slopsix_kls_build(int8_t *offsets, uint8_t breakpoint,
    uint8_t left_depth, uint8_t right_depth,
    uint8_t left_curve, uint8_t right_curve);

/* The breakpoint's key-code group. */
uint8_t slopsix_kls_breakpoint_index(uint8_t breakpoint);

/* The velocity route: the hardware's operator volume attenuation for a
 * velocity and a velocity sensitivity (research E-07).  The attenuation
 * falls as the key is struck harder; the direction is pinned in Phase 3
 * (see the algorithm page). */
int32_t slopsix_velocity_attenuation(uint8_t velocity, uint8_t sensitivity);

/* The operator's level in the Q8 domain: the output level, the keyboard
 * level scaling offset of the note's group and the velocity. */
int32_t slopsix_op_level_q8(uint8_t out_level, int8_t kls_offset,
    uint8_t velocity, uint8_t velocity_sens);

/* The note's key-code group: the index of the keyboard level scaling
 * curve.  Three semitones share a group. */
uint8_t slopsix_note_group(uint8_t note);

/* The rate scaling: the note's contribution to the envelope's rate. */
uint8_t slopsix_rate_scaling(uint8_t note, int32_t pitch_offset,
    uint8_t sensitivity);

/* The EGS master clock's tick threshold for a host rate (zero means the
 * anchor rate; the value is bounded like the cadence gates' rate). */
uint32_t slopsix_eg_tick_threshold(uint32_t sample_rate);

/* The envelope. */
void slopsix_eg_init(slopsix_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4], int32_t op_level_q8,
    uint8_t rate_scaling, uint32_t sample_rate);
void slopsix_eg_note_on(slopsix_eg *eg);
void slopsix_eg_note_off(slopsix_eg *eg);
/* Bring the envelope to the domain's silence floor and the finished
 * stage, as the firmware's patch activation does to every voice. */
void slopsix_eg_silence(slopsix_eg *eg);
int32_t slopsix_eg_step(slopsix_eg *eg);

/* The envelope stepped `count` times, one result per sample, leaving
 * the same state `count` slopsix_eg_step calls would leave (the block
 * renderer's level fill). */
void slopsix_eg_fill(slopsix_eg *eg, int16_t *out, unsigned count);

#endif /* SLOPSIX_INTERNAL_H */
