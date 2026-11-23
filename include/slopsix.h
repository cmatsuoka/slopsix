/*
 * slopsix.h - public interface of libslopsix.
 *
 * A dependency-free, integer-only, fixed-point FM synthesis library that
 * emulates the DX7 family of instruments.  The library renders into
 * caller-supplied buffers, performs no I/O, and allocates only when a
 * context is created.
 *
 * Units and conventions:
 *   - Human-facing scalars are Q16: 1.0 == 65536 is one unit of the
 *     parameter's own unit (gain, pan position, cents).
 *   - Parameters whose value the hardware itself encodes keep that
 *     integer's own format (bend 14-bit, controller 0..127, master
 *     volume 0..7).
 *   - Every parameter documents its unit and sign convention where it is
 *     declared.
 *
 * Engines (personalities):
 *   - The engine is chosen at context creation and never changes.  The
 *     compile-time feature macros decide which engines are present in a
 *     build; the creation parameter chooses among those present, and a
 *     request for an absent engine fails at creation (NULL) instead of
 *     failing later at render time.
 *   - The DX7 is always present and is the default engine.
 *   - The DX7S personality is experimental; the SLOPSIX_HAS_DX7S
 *     capability macro reports it as absent when the personality is not
 *     compiled in.
 *
 * This header is the only public header; internals live in
 * src/slopsix_internal.h.
 */
#ifndef SLOPSIX_H
#define SLOPSIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Library version. major/minor/patch are packed as 0x00MMmmpp.
 * The plain integer constants keep SLOPSIX_VERSION_NUMBER usable in #if. */
#define SLOPSIX_VERSION_MAJOR 0
#define SLOPSIX_VERSION_MINOR 3
#define SLOPSIX_VERSION_PATCH 0
#define SLOPSIX_VERSION_NUMBER \
    ((SLOPSIX_VERSION_MAJOR << 16) | \
        (SLOPSIX_VERSION_MINOR << 8) | SLOPSIX_VERSION_PATCH)

/* Capability macros: what this build can do.  The host can test them at
 * compile time, so a program that uses an optional feature can be built
 * for a build without it.
 *
 * SLOPSIX_HAS_DX7S follows the personality's own controlling macro: a
 * build that selects the DX7S configuration (SLOPSIX_DX7S) also compiles
 * the personality's translation units, so the capability reports 1; a
 * build without the macro reports 0 and rejects that engine at
 * creation. */
#define SLOPSIX_HAS_DX7 1
#if defined(SLOPSIX_DX7S)
#define SLOPSIX_HAS_DX7S 1
#else
#define SLOPSIX_HAS_DX7S 0
#endif


/* Optional features beyond the original instruments.  The pan control is
 * the first of these; with SLOPSIX_EXTRAS off the synths stay mono at
 * centre, as their hardware does. */
#if defined(SLOPSIX_EXTRAS)
#define SLOPSIX_HAS_EXTRAS 1
#else
#define SLOPSIX_HAS_EXTRAS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes.  Every setter and every note event returns one, so that a
 * call shared across the engines can say "this engine does not have that
 * feature" (SLOPSIX_ERR_UNSUPPORTED) instead of silently doing nothing.
 *
 * A rejected request is still applied in the clamped form where the
 * hardware would clamp it; the code reports what happened:
 *   SLOPSIX_ERR_RANGE      the value was out of range and was clamped.
 *                          Where there is no clamped form - the patch
 *                          I/O's dump size - the call is rejected and
 *                          nothing changes
 *   SLOPSIX_ERR_UNSUPPORTED the engine does not have the feature; the
 *                          value is left unchanged
 *   SLOPSIX_ERR_INVALID    the call itself was malformed (a null context,
 *                          a null buffer, an unknown source) */
typedef enum {
    SLOPSIX_OK = 0,
    SLOPSIX_ERR_UNSUPPORTED = 1,  /* feature/parameter not available here */
    SLOPSIX_ERR_RANGE = 2,        /* value out of range (clamped) */
    SLOPSIX_ERR_INVALID = 3       /* malformed input */
} slopsix_error;

/* Return the version of the library the process is linked against, packed as
 * SLOPSIX_VERSION_NUMBER. This reports the compiled library, so a host can
 * compare it with the header's SLOPSIX_VERSION_NUMBER to detect a mismatched
 * build. */
uint32_t slopsix_version_number(void);

/* The context is opaque: it is created, configured and rendered. */
typedef void *slopsix_context;

/* The engines (personalities).  The count is the number of known engines,
 * not the number of engines present in a build. */
typedef enum {
    SLOPSIX_ENGINE_DX7 = 0,
    SLOPSIX_ENGINE_DX7S = 1,
    SLOPSIX_ENGINE_COUNT = 2
} slopsix_engine;

/* ---------------------------------------------------------------- context */

/* Allocate and initialize a context for `engine`.  0 selects the engine's
 * default rate (49,096 Hz for the DX7).  Returns NULL on failure,
 * including when the requested engine is not compiled into this build
 * (see SLOPSIX_HAS_DX7S). */
slopsix_context slopsix_create_context(slopsix_engine engine,
    uint32_t sample_rate);

/* Release a context.  NULL is ignored.  Release exactly once. */
void slopsix_destroy_context(slopsix_context ctx);

/* Change the sample rate (rebuilds the rate-derived tables) or reset
 * voices, controllers and mix to their defaults. */
void slopsix_set_sample_rate(slopsix_context ctx, uint32_t sample_rate);
void slopsix_reset(slopsix_context ctx);

/* ---------------------------------------------------------------- render */

/* Render `frames` stereo interleaved samples into `stereo_out`
 * (2 * frames int16_t values).  Deterministic.  The render entry point is
 * resolved at context creation, so this is one indirect call per buffer
 * and no personality branch sits in the sample loop.
 *
 * The output level is a library convention, not a hardware fact (the
 * DAC's absolute span and the board's analog stage are deliberately out
 * of the model).  At the default master volume - the hardware's top
 * ladder tap - the 12-bit code window's rails sit 6 dB below int16 full
 * scale, leaving room for the analog tail's passband and transient
 * overshoot and for ordinary polyphony; the conversion saturates only at
 * the int16 rails.  This matches the VDX7 reference renderer's gain
 * staging. */
void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames);

/* ---------------------------------------------------------------- patches */

/* Load a voice dump into the live voice.  `size` is the dump's length:
 * the engine's own size (156 bytes for the DX7).  A mismatch is reported
 * as SLOPSIX_ERR_RANGE and the dump is not loaded.  Values are taken
 * as-is (checksums and SYSEX framing are host concerns).
 *
 * As on the hardware, a patch load silences any voice that is already
 * sounding: the patch activation resets the voice chips before the new
 * registers take effect.  A key still held keeps its allocated voice,
 * but that voice stays silent until the key is released and struck
 * again. */
slopsix_error slopsix_set_voice(slopsix_context ctx, const uint8_t *data,
    size_t size);

/* Read back the live voice block; `size` must be the engine's dump size,
 * otherwise SLOPSIX_ERR_RANGE is reported and nothing is written. */
slopsix_error slopsix_get_voice(const slopsix_context ctx, uint8_t *out,
    size_t size);

/* ------------------------------------------------------- voice management */

/* Note on.  velocity is 0..127 and feeds the per-operator
 * velocity-sensitivity route (the DX7's own route: the keybed is
 * velocity sensitive, and the patch's per-operator sensitivity decides
 * how much each operator follows it).  A host that sends a fixed value
 * reproduces the behaviour of a keybed that never varies its velocity.
 *
 * A note above 127 is clamped to 127 and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_note_on(slopsix_context ctx, uint8_t note,
    uint8_t velocity);
/* Note off.  A note above 127 is clamped to 127 and reported as
 * SLOPSIX_ERR_RANGE, as on note on; releasing a key the engine is not
 * holding changes nothing. */
slopsix_error slopsix_note_off(slopsix_context ctx, uint8_t note);
/* All Notes Off (CC 123 semantics): every key is released, so voices held
 * by the sustain pedal keep ringing until pedal-up. */
slopsix_error slopsix_all_notes_off(slopsix_context ctx);

/* Sustain pedal: held notes defer their release. */
slopsix_error slopsix_set_sustain(slopsix_context ctx, bool on);

/* Poly/mono.  In mono mode the last-pressed-note priority and the legato
 * behaviour follow the hardware's function panel. */
slopsix_error slopsix_set_mono_mode(slopsix_context ctx, bool mono);

/* DX7S LFO distribution.  Single is the Mk1's one instrument-wide LFO;
 * Multi gives every voice its own LFO with the same settings and an
 * independent phase/delay history.  The parameter is not part of the
 * 156-byte Mk1 dump, so it defaults to Single.  An engine without the
 * DX7S personality reports SLOPSIX_ERR_UNSUPPORTED. */
typedef enum {
    SLOPSIX_LFO_SINGLE = 0,
    SLOPSIX_LFO_MULTI = 1
} slopsix_lfo_mode;

slopsix_error slopsix_set_lfo_mode(slopsix_context ctx,
    slopsix_lfo_mode mode);

/* -------------------------------------------------- DX7S-only parameters
 *
 * The DX7S adds a per-voice key mode, unison detune and three pitch-EG
 * parameters.  They are not part of the 156-byte DX7 voice dump, so they
 * are set through the API and keep their Mk1-equivalent defaults.  A
 * context whose engine does not have them reports SLOPSIX_ERR_UNSUPPORTED
 * and leaves the value unchanged: this is what keeps a DX7 context from
 * acquiring DX7S-only behaviour (and sounding wrong) when the host shares
 * one patch/parameter path across the personalities.
 *
 * The values mirror the DX7S's own encodings, exactly as the function
 * panel and the additional-voice SysEx do. */

/* The four key modes (owner's manual: Poly 16 notes, Mono 1 note with one
 * voice, Unison Poly 4 notes with four voices each, Unison Mono 1 note
 * with all sixteen voices). */
typedef enum {
    SLOPSIX_KEY_POLY = 0,          /* 16 independent notes */
    SLOPSIX_KEY_MONO,              /* one note, one voice */
    SLOPSIX_KEY_UNISON_POLY,       /* four notes, four voices each */
    SLOPSIX_KEY_UNISON_MONO        /* one note, sixteen voices */
} slopsix_key_mode;

/* An out-of-range enumerator reports SLOPSIX_ERR_INVALID and leaves the
 * mode unchanged.  Poly is the default, so a neutral DX7S context matches
 * the DX7's voice allocation. */
slopsix_error slopsix_set_key_mode(slopsix_context ctx, slopsix_key_mode mode);

/* Unison detune, 0..7, active only in the two unison modes.  A larger
 * value is clamped to 7 and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_unison_detune(slopsix_context ctx,
    uint8_t depth_0_7);

/* The pitch EG's four range settings, in the DX7S's data encoding: the
 * additional-voice SysEx carries the raw value in this order, so a host
 * passes the hardware's value to the library unchanged.  The DX7S divides
 * the signed level offset by 1/4/16/64, i.e. the spans are 8, 2, 0.5 and
 * 0.125 octaves. The enumerator names keep the manual's labels: the third
 * and fourth are the chart's 1/2-octave value and the smallest step. 8
 * octaves is the Mk1-equivalent default (the DX7's fixed span). */
typedef enum {
    SLOPSIX_PEG_RANGE_8_OCTAVES = 0,    /* the Mk1's fixed span */
    SLOPSIX_PEG_RANGE_2_OCTAVES,        /* 2 octaves */
    SLOPSIX_PEG_RANGE_1_OCTAVE,         /* 1/2 octave (6 semitones) */
    SLOPSIX_PEG_RANGE_HALF_OCTAVE       /* 1/8 octave (1.5 semitones) */
} slopsix_peg_range;

/* An out-of-range enumerator reports SLOPSIX_ERR_INVALID and leaves the
 * range unchanged. */
slopsix_error slopsix_set_peg_range(slopsix_context ctx,
    slopsix_peg_range range);

/* Pitch-EG velocity sensitivity: a switch, not a continuous depth. */
slopsix_error slopsix_set_peg_velocity(slopsix_context ctx, bool on);

/* Pitch-EG rate scaling, 0..7.  A larger value is clamped to 7 and
 * reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_peg_rate_scaling(slopsix_context ctx,
    uint8_t depth_0_7);

/* Per-operator amplitude modulation sensitivity, 0..7 (operator 1..6).
 * The DX7S widens the voice data's 2-bit field (0..3) to three bits and
 * carries the wide value in its additional voice data; the voice data
 * holds min(value, 3), and 0..3 mean what they mean on the Mk1.  Loading
 * voice data (slopsix_set_voice) resets every operator to the dump's
 * 2-bit value, so apply this afterwards.  The value takes effect at the
 * next note-on.  An operator outside 1..6 reports SLOPSIX_ERR_INVALID.
 * On the DX7S a sensitivity above 7 is clamped to 7 and reported as
 * SLOPSIX_ERR_RANGE; a context whose engine is not the DX7S reports
 * SLOPSIX_ERR_UNSUPPORTED.  The DX7S carries the three-bit value in the
 * same per-operator sensitivity field the Mk1 uses, so 4..7 continue the
 * Mk1's doubling per step and saturate at the 12-bit attenuation floor;
 * there is no separate high-range curve. */
slopsix_error slopsix_set_amp_mod_sensitivity(slopsix_context ctx,
    uint8_t op_1_6, uint8_t sens_0_7);

/* Digital output gain: when true, the DX7S analog restore gain is
 * halved (x4 instead of x8), bringing the output to the same level
 * as the DX7 Mk1.  The default is false (-6 dB below the Mk1,
 * matching the real-device measurement).  A context whose engine
 * is not the DX7S reports SLOPSIX_ERR_UNSUPPORTED and leaves the
 * value unchanged. */
slopsix_error slopsix_set_digital_output_gain(slopsix_context ctx,
    bool normalize);

/* ------------------------------------------------------------- portamento */

/* The hardware's two portamento modes.  In fingered mode a legato press
 * glides without the pedal; in full-time mode the pedal must be held, and
 * a new note also drags pedal-sustained and released voices along. */
typedef enum {
    SLOPSIX_PORTA_FINGERED = 0,
    SLOPSIX_PORTA_FULL_TIME
} slopsix_portamento_mode;

/* Portamento time, 0..99 as on the function panel.  0 is the
 * instantaneous setting; a larger value lengthens the glide (the hardware
 * derives a rate increment from its pitch-EG rate table).  A larger value
 * is clamped and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_portamento_time(slopsix_context ctx,
    uint8_t time_0_99);

/* Portamento mode.  An out-of-range enumerator reports
 * SLOPSIX_ERR_INVALID and leaves the mode unchanged. */
slopsix_error slopsix_set_portamento_mode(slopsix_context ctx,
    slopsix_portamento_mode mode);

/* Portamento pedal.  In polyphonic mode the pedal gates portamento
 * entirely; in full-time mono mode it does too, while fingered mono mode
 * glides on legato presses regardless. */
slopsix_error slopsix_set_portamento_pedal(slopsix_context ctx, bool on);

/* Glissando: quantise the glide into semitone steps, as the hardware's
 * GLISSANDO function parameter does. */
slopsix_error slopsix_set_glissando(slopsix_context ctx, bool on);

/* ------------------------------------------------------------------ pitch */

/* Pitch bend: 14-bit, 0x2000 centred (MIDI convention).  The engine
 * reduces it to the hardware's 7-bit bend and applies the DX7's own bend
 * curve, including its centre dead zone and its travel asymmetry.
 * A value above 16383 is clamped and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_pitch_bend(slopsix_context ctx, uint16_t bend);

/* Bend range in semitones (0..12; 0 disables bending) and bend step
 * (0..15; 0 = continuous).  Both are global, as on the hardware.  A
 * non-zero step quantises the bend into the hardware's staircase and
 * makes the range irrelevant - the hardware then uses the full +/- 12.
 * A step of 13..15 reaches the step table's zero groups and bends
 * nothing; the hardware's own validation never leaves such a value in
 * the register.  A step above 15 or a range above 12 is reported as
 * SLOPSIX_ERR_RANGE and the parameter takes the hardware's validation
 * form (0).
 *
 * The step is a DX7-family parameter: an engine without it reports
 * SLOPSIX_ERR_UNSUPPORTED and leaves the bend unchanged. */
slopsix_error slopsix_set_pitch_bend_range(slopsix_context ctx,
    uint8_t range_semitones,
    uint8_t step);

/* Master tune: Q16 cents, 1.0 == 1 cent.  0 is the instrument's shipped
 * tuning (MIDI 69 == 440 Hz), negative is flat, positive is sharp.  The
 * engine quantizes the value to its internal 1/4096-octave grid, about
 * 0.29 cents. */
slopsix_error slopsix_set_master_tune(slopsix_context ctx, int32_t cents_q16);

/* ------------------------------------------------------------ controllers */

typedef enum {
    SLOPSIX_CTRL_MOD_WHEEL = 0,
    SLOPSIX_CTRL_FOOT,
    SLOPSIX_CTRL_BREATH,
    SLOPSIX_CTRL_AFTERTOUCH,
    SLOPSIX_CTRL_SRC_COUNT = 4
} slopsix_ctrl_source;

/* Controller value, 0..127, scaled by the source's range.  A larger value
 * is clamped and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_ctrl_value(slopsix_context ctx,
    slopsix_ctrl_source src,
    uint8_t value_0_127);

/* Controller range, 0..99, as on the hardware's function panel.  A larger
 * value is clamped and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_ctrl_range(slopsix_context ctx,
    slopsix_ctrl_source src,
    uint8_t range_0_99);

/* Routing flags, OR-ed: SLOPSIX_ASSIGN_PITCH, SLOPSIX_ASSIGN_AMP,
 * SLOPSIX_ASSIGN_EG.  Unknown bits are masked off and reported as
 * SLOPSIX_ERR_RANGE. */
#define SLOPSIX_ASSIGN_PITCH 0x1u
#define SLOPSIX_ASSIGN_AMP   0x2u
#define SLOPSIX_ASSIGN_EG    0x4u

slopsix_error slopsix_set_ctrl_assign(slopsix_context ctx,
    slopsix_ctrl_source src,
    unsigned flags);

/* ------------------------------------------------------------------- mix */

/* Per-voice-bus pan position, Q16: 0 is hard left, 65536 is hard right,
 * 32768 is centre (the default: identical left and right).
 *
 * This is an extra, not hardware behaviour: every supported instrument
 * has a mono output.  It is available only when the library is compiled
 * with SLOPSIX_EXTRAS (SLOPSIX_HAS_EXTRAS); without it the call is a
 * no-op, the output stays at centre and the call reports
 * SLOPSIX_ERR_UNSUPPORTED. */
slopsix_error slopsix_set_pan(slopsix_context ctx, int32_t pan_q16);

/* The hardware's 8 discrete master volume steps (0..7).  A larger value
 * is clamped to 7 and reported as SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_midi_volume(slopsix_context ctx,
    unsigned level_0_7);

/* The host's output gain, Q16: 0 is silence and 65536 is unity (the
 * default).  It is a plain output gain, applied with the
 * hardware's 8-step master volume in the final conversion's single
 * multiply; it neither alters the modeled DAC reference ladder nor the
 * analog tail.  A value outside 0..65536 is clamped and reported as
 * SLOPSIX_ERR_RANGE. */
slopsix_error slopsix_set_output_gain(slopsix_context ctx,
    int32_t gain_q16);

/* ----------------------------------------------------------- capabilities */

/* The engine a context was created for.  A null context answers
 * SLOPSIX_ENGINE_COUNT, not 0 (0 is the DX7). */
slopsix_engine slopsix_context_engine(const slopsix_context ctx);

/* The engine's shape: its polyphony, its operator count, its algorithm
 * count and its voice dump's size in bytes (156 for the DX7).  A null
 * context answers 0. */
size_t slopsix_voice_count(const slopsix_context ctx);
size_t slopsix_operator_count(const slopsix_context ctx);
size_t slopsix_algorithm_count(const slopsix_context ctx);
size_t slopsix_voice_dump_size(const slopsix_context ctx);

/* The context's sample rate. */
uint32_t slopsix_context_sample_rate(const slopsix_context ctx);

#ifdef __cplusplus
}
#endif

#endif /* SLOPSIX_H */
