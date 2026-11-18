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
extern const uint8_t slopsix_note_t[128];
extern const uint16_t slopsix_note_word[128];
extern const uint8_t slopsix_com_log[6];
extern const int32_t slopsix_coarse_offset[32];
extern const int16_t slopsix_fine_offset[100];

/* The ROM's tables the modulation paths read (checked in verbatim;
 * src/slopsix_lfo.c). */
extern const uint8_t slopsix_lfo_sin_table[64];     /* the sine LFO's wave */
extern const uint8_t slopsix_amp_mod_table[256];    /* the amp-mod curve */
extern const uint8_t slopsix_pitch_mod_sens[8];     /* the PMS table */

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

/* Evidence grade: provisional (research A-02, A-04).
 * The sum/delay register's membership rule.  The hardware has one
 * sum/delay register; the MREN control bits that decide which operator
 * outputs enter it are not decoded yet, so the working model is used:
 * an operator whose D bit is set writes the first scratch bus, one whose
 * C bit is set writes the second, and the register reads the sum of the
 * two buses (counting an operator that wrote both once).
 *
 * The ROM's first control bit is read as "this operator's output joins
 * the feedback path".  That reading agrees with the analysed engine's
 * feedback structure in 30 of the 32 algorithms; in algorithms 4 and 6
 * the bit sits on operator 4, which the reading takes as an extra
 * contributor to the feedback path.  The feedback operator itself is
 * always the one whose modulation selector is the self-feedback source,
 * which agrees with the decoded table in all 32 algorithms. */
#define SLOPSIX_BUS_FIRST 1u
#define SLOPSIX_BUS_SECOND 2u

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
 * (research OU-02, OQ-24).  The mixed bus's master DAC stage reduces the
 * bus to the hardware's 12-bit DAC code grid.  The bus's unit is a
 * quantized voice's value (mantissa << shift), whose full scale is
 * 8 * 2047, so the code grid sits at one eighth of the bus unit - the
 * same domain the compander's 12-bit datum occupies - and the sum is
 * saturated at the code window's rails instead of wrapping.  The
 * hardware's true saturation level is set by the board's analog stage
 * and stays open; the rails are the engine's pinning. */
#define SLOPSIX_DAC_GRID_SHIFT 3u
#define SLOPSIX_DAC_CODE_MAX 2047
#define SLOPSIX_DAC_CODE_MIN (-2048)

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

typedef struct {
    slopsix_voice_state state;
    bool held;          /* key still down; false while pedal-sustained */
    uint8_t note;       /* 0..127 */
    uint8_t velocity;   /* 0..127 */
    int8_t transpose;   /* the patch transpose keyed with the note */
    uint32_t seq;       /* the voice allocation's sequence counter; the
                         * stealing picks the lowest (the oldest note-on) */
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
    uint8_t ams_sens;       /* the hardware's 2-bit field, 0..3 */
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
} slopsix_patch_state;

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
 * engine is anchored. */
#define SLOPSIX_LEVEL_OFFSET_Q8 4256

/* The envelope's own fixed point: the Q8 level with 16 fractional bits.
 * The attack's asymptote and its jump target are the model's constants
 * (research E-01). */
#define SLOPSIX_EG_FRAC 16
#define SLOPSIX_EG_MIN_Q8 16
#define SLOPSIX_EG_JUMP_Q8 1716
#define SLOPSIX_EG_MAX_Q8 4352

/* The envelope's block cadence: the reference model advances the
 * envelope once per 64-sample block and interpolates the gain across
 * it.  The engine keeps both cadences behind one switch until a capture
 * pins the hardware's own (plan decision D-9). */
#define SLOPSIX_EG_BLOCK 64

/* One operator's envelope generator.
 *
 * The state machine walks the four stages (attack, decay, sustain,
 * release); the key-up jumps straight to the release stage.  The level
 * is held in the engine's fixed point and moves towards the current
 * stage's target.
 */
typedef struct {
    /* The parameters, set at note-on. */
    uint8_t rates[4];       /* the four stage rates, 0..99 */
    uint8_t levels[4];      /* the four stage levels, 0..99 */
    int32_t op_level_q8;    /* the operator's own level (Q8) */
    uint8_t rate_scaling;   /* the note's rate scaling delta, 0..31 */

    /* The runtime state. */
    int32_t level;          /* the current level, Q8 << SLOPSIX_EG_FRAC */
    int32_t target;         /* the current stage's target */
    int32_t inc;            /* the current stage's increment */
    int32_t static_count;   /* the remaining static-count samples */
    uint8_t stage;          /* 0..3, 4 when the envelope has finished */
    uint8_t active;         /* the envelope is in use */
    uint8_t rising;         /* the stage rises towards its target */
    uint8_t down;           /* the key is down */
    uint8_t block_mode;     /* the cadence switch (plan decision D-9) */
    uint8_t block_pos;      /* the sample position inside the block */
    int32_t block_level;    /* the block's starting level */
    int32_t block_step;     /* the block's per-sample step */
    uint32_t sample_rate;   /* the host's sample rate */
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
    uint8_t ams_sens;      /* the amplitude modulation sensitivity, 0..3 */
    int8_t kls[SLOPSIX_KLS_GROUPS]; /* the keyboard level scaling offsets,
                                     * one per key-code group */
} slopsix_op_params;

/* ------------------------------------------------- the operator graph */

/* Where an operator's modulation input comes from.  The DX7
 * configuration uses the hardware's five selector sources (research
 * A-01); the general graph leaves room for more. */
typedef enum {
    SLOPSIX_MOD_NONE = 0,
    SLOPSIX_MOD_OPERATOR,          /* the operator computed just before */
    SLOPSIX_MOD_FEEDBACK,          /* the operator's own feedback path */
    SLOPSIX_MOD_REGISTER,          /* the sum/delay register */
    SLOPSIX_MOD_REGISTER_DELAYED,  /* the register's previous sample */
    SLOPSIX_MOD_FEEDBACK_DELAYED   /* the feedback path's delayed value */
} slopsix_mod_source;

/* One operator's connection in the graph. */
typedef struct {
    slopsix_mod_source source;
    uint8_t source_op;  /* the modulating operator, 1..6, when relevant */
    uint8_t carrier;    /* the output joins the voice's carrier sum */
    uint8_t com;        /* the carrier-count compensation code, 0..5 */
    uint8_t buses;      /* the scratch buses the output is written to */
    uint8_t fb_out;     /* the output joins the feedback path */
} slopsix_op_conn;

/* One algorithm's connection data. */
typedef struct {
    slopsix_op_conn ops[SLOPSIX_OP_COUNT];  /* index 0 == operator 1 */
} slopsix_algorithm;

/* A voice's engine state: the operators, their parameters and their
 * frequencies.  The walk's bus state is local to the render call; only
 * the delayed state (the sum/delay register and the feedback path's
 * two registers) and the operators' last samples survive a sample. */
typedef struct {
    slopsix_operator ops[SLOPSIX_OP_COUNT];
    slopsix_op_params params[SLOPSIX_OP_COUNT];
    slopsix_eg eg[SLOPSIX_OP_COUNT];  /* the operators' envelopes */
    slopsix_pitch_eg pitch_eg;        /* the voice's pitch envelope */
    int32_t out[SLOPSIX_OP_COUNT];  /* the operators' last samples */
    int32_t reg_delay;              /* the register's previous sample */
    int32_t fb_prev;                /* the feedback path's last sum */
    int32_t fb_prev2;               /* the feedback path's sum before it */
    int32_t fb_delay;               /* the feedback path's delayed value */
    uint8_t feedback_level;         /* the OPS's 3-bit feedback field */
    int32_t pitch_offset;           /* the last applied log-frequency
                                     * offset (bend + LFO + pitch EG); the
                                     * render gate skips a rewrite when it
                                     * is unchanged */
    /* The LFO's amplitude modulation reaches the walk as a per-tick,
     * instrument-wide register value (src/slopsix_lfo.c), so it is not
     * cached per voice. */
} slopsix_voice_engine;

/* --------------------------------------------------------- context state */

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

    /* Voice management. */
    bool sustain;
    bool mono;

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
 * The hardware averages the last two samples of the feedback path before
 * feeding them back (the die's two feedback registers and the averaging
 * adder), and the result is divided by a power of two (the 0..6-bit
 * feedback shifter).  The level-to-shift mapping is 7 - level, which a
 * bit-synchronous OPS model corroborates (research O-06); the path's own
 * value is the sum of the outputs that the algorithm's control bits add
 * to it.
 */
static inline int32_t slopsix_feedback_scaled(int32_t fb_prev,
    int32_t fb_prev2, uint8_t feedback_level)
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

    return value;
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
    uint32_t acc;
    uint32_t phase;
    uint32_t sine_phase;
    int32_t log_value;
    int32_t sample;

    /* The modulation input - the sum of the operator's input buses, or
     * the feedback path's value - is added to the phase for the lookup
     * only: it is not part of the accumulator's state.  It is not
     * clamped: the hardware adds the operator's linear output (or the
     * modulator sum) as it is, and the phase addition wraps. */
    acc = op->phase;
    phase = acc + (uint32_t)(mod_in * (1 << SLOPSIX_MOD_DEVIATION_SHIFT));

    sine_phase = (phase >> SLOPSIX_SINE_PHASE_SHIFT) & SLOPSIX_SINE_PHASE_MASK;

    /* The log word: the operator's pre-shifted log amplitude (Q10) minus
     * the log-sine's negated log2 value. */
    log_value = log_amp_q10 - slopsix_logsin(sine_phase);

    sample = slopsix_signal_exp(log_value);

    if (sine_phase & SLOPSIX_SINE_HALF_BIT) {
        /* The negative half of the wave inverts the output. */
        sample = -sample;
    }

    op->phase = (acc + op->increment) & SLOPSIX_PHASE_MASK;

    *out = sample;
}

/* ------------------------------------------------- the render composites */

/*
 * The carrier-count compensation, in the Q8 domain: a negative value
 * that the carrier's log amplitude carries (research A-05).  The code
 * is static per algorithm, so it is composed on the render path per
 * sample; it is inline for the same reason as the helpers above.
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
 * modulated).  Inline for the same reason as the helpers above.
 */
static inline int32_t slopsix_ams_reduction_q8(uint8_t ams_sens,
    uint8_t amp_mod_register)
{
    if (ams_sens == 0u) {
        return 0;
    }
    if (ams_sens > 3u) {
        ams_sens = 3u;
    }
    return (int32_t)amp_mod_register << ams_sens;
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

/* ----------------------------------------------------- output (Phase 5) */

/* The output chain mirrors the hardware's (algorithm page §8, plan
 * decision D-5): the per-voice carrier sum is quantized by the compander
 * into a 12-bit mantissa plus a 3-bit shift, the quantized voices sum on
 * a wide bus, the sum passes the master DAC's 12-bit code step with
 * saturation, the optional pan splits the mono result (SLOPSIX_EXTRAS),
 * and the 3-bit master volume scales the DAC's reference before the
 * int16 conversion.  The stages are distinct functions with their own
 * fixed widths, so a later extension can replace the quantization or the
 * conversion without touching the mixer (plan decision D-15).
 *
 * The compander runs per voice, so it is inline here like the operator
 * core; the once-per-sample stages live in src/slopsix_output.c. */

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

/* The quantized word's value, in the bus's unit (the truncated input). */
static inline int32_t slopsix_compander_value(const slopsix_compander_word *word)
{
    return (int32_t)word->mantissa << word->shift;
}

/* The master DAC step: the mixed bus reduced to the 12-bit code grid,
 * saturated at the rails (the constants above carry the model's grade). */
int32_t slopsix_dac_step(int32_t bus);

/* The master volume's gain: the hardware's 8-level DAC reference ladder,
 * level 0 the quietest tap, level 7 the top tap, as a Q16 fraction of
 * the top tap. */
int32_t slopsix_master_gain_q16(unsigned level);

/* The final conversion: one DAC code times a Q16 gain, rounded and
 * mapped onto int16.  With the DAC's rails in front of it the conversion
 * itself cannot overflow int16: the chain's single saturation is the DAC
 * step's. */
int16_t slopsix_output_scale(int32_t code, int32_t gain_q16);

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
 * compile-time-selected pan and the master conversion, from the mixed
 * bus into the two int16 channels.  No runtime mode flag, no branch per
 * personality (plan decision D-18's rule on the output path). */
void slopsix_output_mix(const slopsix_context_state *state, int32_t bus,
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

/* The LFO's life cycle: the shared cadence gate (above) and the note-on
 * hook - the firmware's VOICE_ADD side, which clears the delay
 * accumulator and the fade factor when the patch's delay is not 0, and
 * seeds the phase accumulator to 0x7FFF when the patch's LFO SYNC is set.
 * The LFO state is global, so the hook runs on every note-on, as the
 * firmware's voice-add paths do. */
void slopsix_lfo_note_on(slopsix_context_state *state);

/* The patch activation's LFO parse: the dump's bytes 137..143 through the
 * firmware's own activation scaling - the two closed laws for the
 * increments, the shared value scale's high bytes for the depths, the
 * PMS table for the sensitivity and the raw byte for the waveform (the
 * algorithm page §6.6). */
void slopsix_lfo_patch_activate(slopsix_context_state *state,
    const uint8_t *dump);

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
 * engine's 4096-per-octave units (neutral 0, about -4..+3.97 octaves). */
int32_t slopsix_pitch_eg_offset(const slopsix_pitch_eg *eg);

/* The pitch envelope's life cycle. */
void slopsix_pitch_eg_init(slopsix_pitch_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4]);
void slopsix_pitch_eg_note_on(slopsix_pitch_eg *eg);
void slopsix_pitch_eg_note_off(slopsix_pitch_eg *eg);

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
                                  uint8_t amp_mod_register,
                                  int32_t *voice_out);

/* The voice path (Phase 5).  The note-on fills one voice's engine from
 * the live patch, keying it as the firmware's VOICE_ADD does (envelopes
 * out of silence, the pitch EG reset to its initial level, the phases
 * reset when the patch's OSC KEY SYNC bit is set); the release does the
 * VOICE_REMOVE side; the frequency rewrite composes the effective note,
 * the master tune and the voice's current pitch offset into every
 * operator's phase increment, as the firmware's PORTA_PROCESS does at
 * its every-second-tick cadence. */
void slopsix_voice_engine_note_on(slopsix_context_state *state,
    slopsix_voice *slot, slopsix_voice_engine *engine);
void slopsix_voice_engine_release(slopsix_voice_engine *engine);
void slopsix_voice_engine_update_frequency(slopsix_context_state *state,
    const slopsix_voice *slot, slopsix_voice_engine *engine);
void slopsix_refresh_pitch(slopsix_context_state *state);

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
uint8_t slopsix_rate_scaling(uint8_t note, uint8_t sensitivity);

/* The envelope. */
void slopsix_eg_init(slopsix_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4], int32_t op_level_q8,
    uint8_t rate_scaling, uint32_t sample_rate);
void slopsix_eg_note_on(slopsix_eg *eg);
void slopsix_eg_note_off(slopsix_eg *eg);
int32_t slopsix_eg_step(slopsix_eg *eg);

#endif /* SLOPSIX_INTERNAL_H */
