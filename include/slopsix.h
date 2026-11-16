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
 * This header is the only public header; internals live in
 * src/slopsix_internal.h.
 */
#ifndef SLOPSIX_H
#define SLOPSIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes.  Setters that can reject a request report it this way;
 * the exact reporting shape of the setters is still being decided
 * (plan decision D-7), so the void-returning setters below currently
 * clamp silently. */
typedef enum {
    SLOPSIX_OK = 0,
    SLOPSIX_ERR_UNSUPPORTED = 1,  /* voice/parameter pair not supported */
    SLOPSIX_ERR_RANGE = 2,        /* value out of range (clamped) */
    SLOPSIX_ERR_INVALID = 3       /* malformed input */
} slopsix_error;

/* The context is opaque: it is created, configured and rendered. */
typedef void *slopsix_context;

/* ---------------------------------------------------------------- context */

/* Allocate and initialize a context.  0 selects the default rate
 * (49,096 Hz).  Returns NULL on failure. */
slopsix_context slopsix_create_context(uint32_t sample_rate);

/* Release a context.  NULL is ignored.  Release exactly once. */
void slopsix_destroy_context(slopsix_context ctx);

/* Change the sample rate (rebuilds the rate-derived tables) or reset
 * voices, controllers and mix to their defaults. */
void slopsix_set_sample_rate(slopsix_context ctx, uint32_t sample_rate);
void slopsix_reset(slopsix_context ctx);

/* ---------------------------------------------------------------- render */

/* Render `frames` stereo interleaved samples into `stereo_out`
 * (2 * frames int16_t values).  Deterministic. */
void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames);

/* ---------------------------------------------------------------- patches */

/* Load a 156-byte voice dump into the live voice.  Values are taken as-is
 * (checksums and SYSEX framing are host concerns). */
bool slopsix_set_voice(slopsix_context ctx, const uint8_t *voice_156);

/* Read back the live voice block. */
bool slopsix_get_voice(const void *ctx, uint8_t *out_voice_156);

/* ------------------------------------------------------- voice management */

/* Note on.  velocity is 0..127 and feeds the per-operator
 * velocity-sensitivity route (the DX7's own route: the keybed is
 * velocity sensitive, and the patch's per-operator sensitivity decides
 * how much each operator follows it).  A host that sends a fixed value
 * reproduces the behaviour of a keybed that never varies its velocity. */
void slopsix_note_on(slopsix_context ctx, uint8_t note, uint8_t velocity);
void slopsix_note_off(slopsix_context ctx, uint8_t note);
/* All Notes Off (CC 123 semantics): every key is released, so voices held
 * by the sustain pedal keep ringing until pedal-up. */
void slopsix_all_notes_off(slopsix_context ctx);

/* Sustain pedal: held notes defer their release. */
void slopsix_set_sustain(slopsix_context ctx, bool on);

/* Poly/mono.  In mono mode the last-pressed-note priority and the legato
 * behaviour follow the hardware's function panel. */
void slopsix_set_mono_mode(slopsix_context ctx, bool mono);

/* ------------------------------------------------------------------ pitch */

/* Pitch bend: 14-bit, 0x2000 centred (MIDI convention).  The engine
 * reduces it to the hardware's 7-bit bend and applies the DX7's own bend
 * curve, including its centre dead zone and its travel asymmetry. */
void slopsix_set_pitch_bend(slopsix_context ctx, uint16_t bend);

/* Bend range in semitones (0..12; 0 disables bending) and bend step
 * (0..15; 0 = continuous).  Both are global, as on the hardware.  A
 * non-zero step quantises the bend into the hardware's staircase and
 * makes the range irrelevant - the hardware then uses the full +/- 12. */
void slopsix_set_pitch_bend_range(slopsix_context ctx,
                                  uint8_t range_semitones,
                                  uint8_t step);

/* Master tune: Q16 cents, 1.0 == 1 cent.  0 is the instrument's shipped
 * tuning (MIDI 69 == 440 Hz), negative is flat, positive is sharp.  The
 * engine quantizes the value to its internal 1/4096-octave grid, about
 * 0.29 cents. */
void slopsix_set_master_tune(slopsix_context ctx, int32_t cents_q16);

/* ------------------------------------------------------------ controllers */

typedef enum {
    SLOPSIX_CTRL_MOD_WHEEL = 0,
    SLOPSIX_CTRL_FOOT,
    SLOPSIX_CTRL_BREATH,
    SLOPSIX_CTRL_AFTERTOUCH,
    SLOPSIX_CTRL_SRC_COUNT = 4
} slopsix_ctrl_source;

/* Controller value, 0..127, scaled by the source's range. */
void slopsix_set_ctrl_value(slopsix_context ctx,
                            slopsix_ctrl_source src, uint8_t value_0_127);

/* Controller range, 0..99, as on the hardware's function panel. */
void slopsix_set_ctrl_range(slopsix_context ctx,
                            slopsix_ctrl_source src, uint8_t range_0_99);

/* Routing flags, OR-ed: SLOPSIX_ASSIGN_PITCH, SLOPSIX_ASSIGN_AMP,
 * SLOPSIX_ASSIGN_EG. */
#define SLOPSIX_ASSIGN_PITCH 0x1u
#define SLOPSIX_ASSIGN_AMP   0x2u
#define SLOPSIX_ASSIGN_EG    0x4u

void slopsix_set_ctrl_assign(slopsix_context ctx,
                             slopsix_ctrl_source src, unsigned flags);

/* ------------------------------------------------------------------- mix */

/* Per-voice-bus pan position, Q16: 0 is hard left, 65536 is hard right,
 * 32768 is centre (the default: identical left and right). */
void slopsix_set_pan(slopsix_context ctx, int32_t pan_q16);

/* The hardware's 8 discrete master volume steps (0..7). */
void slopsix_set_master_volume(slopsix_context ctx, unsigned level_0_7);

#ifdef __cplusplus
}
#endif

#endif /* SLOPSIX_H */
