/*
 * slopsix.c - context life cycle, public API entry points.
 *
 * The audio path proper lives in the per-sample translation units
 * (slopsix_eg.c, slopsix_pitch.c, slopsix_output.c) and the inline
 * operator core in slopsix_internal.h;
 * this file owns the context, the parameter state and the public entry
 * points.  Nothing here allocates after the context exists.
 */
#include <stdlib.h>
#include <string.h>

#include "slopsix_internal.h"

static void slopsix_context_defaults(slopsix_context_state *state)
{
    memset(state, 0, sizeof(*state));
    state->sample_rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    state->pitch_bend = 0x2000u;
    state->bend_range = 12u;
    state->bend_step = 0u;
    state->master_tune_q16 = 0;
    state->pan_q16 = SLOPSIX_Q16_ONE / 2;
    /* The hardware's own reset value: P_DAC = 7, the top tap of the DAC
     * reference ladder, until MIDI volume changes it (dx7-output
     * research, the volume section). */
    state->master_volume = 7u;
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence,
        state->sample_rate);
}

slopsix_context slopsix_create_context(uint32_t sample_rate)
{
    slopsix_context_state *state;

    state = (slopsix_context_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }

    slopsix_context_defaults(state);
    if (sample_rate != 0u) {
        state->sample_rate = sample_rate;
        slopsix_pitch_eg_cadence_init(&state->pitch_cadence, sample_rate);
    }

    return (slopsix_context)state;
}

void slopsix_destroy_context(slopsix_context ctx)
{
    free(ctx);
}

void slopsix_set_sample_rate(slopsix_context ctx, uint32_t sample_rate)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL || sample_rate == 0u) {
        return;
    }
    state->sample_rate = sample_rate;
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence, sample_rate);
}

void slopsix_reset(slopsix_context ctx)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    uint32_t rate;

    if (state == NULL) {
        return;
    }
    rate = state->sample_rate;
    slopsix_context_defaults(state);
    state->sample_rate = rate;
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence, rate);
}

void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames)
{
    size_t i;

    (void)ctx;

    if (stereo_out == NULL) {
        return;
    }

    /* The voice path arrives in Phase 3; until then the output is
     * silence.  The buffer is filled even without a context, so that the
     * render call always writes deterministic output. */
    for (i = 0; i < frames * 2u; i++) {
        stereo_out[i] = 0;
    }
}

bool slopsix_set_voice(slopsix_context ctx, const uint8_t *voice_156)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL || voice_156 == NULL) {
        return false;
    }
    memcpy(state->voice_dump, voice_156, SLOPSIX_VOICE_DUMP_SIZE);
    return true;
}

bool slopsix_get_voice(const void *ctx, uint8_t *out_voice_156)
{
    const slopsix_context_state *state = (const slopsix_context_state *)ctx;

    if (state == NULL || out_voice_156 == NULL) {
        return false;
    }
    memcpy(out_voice_156, state->voice_dump, SLOPSIX_VOICE_DUMP_SIZE);
    return true;
}

/* The release modes of the voice release helper. */
typedef enum {
    SLOPSIX_REL_NOTE,   /* the voices of one note */
    SLOPSIX_REL_ALL,    /* every sounding voice */
    SLOPSIX_REL_PEDAL   /* the voices the pedal holds */
} slopsix_release_mode;

/*
 * Release voices.
 *
 * A released voice that the sustain pedal holds stays in the ON state
 * with its key marked as lifted; the pedal's release (the pedal-up
 * case) frees the voices it holds.
 */
static void slopsix_release_voices(slopsix_context_state *state,
    slopsix_release_mode mode, uint8_t note)
{
    size_t i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        slopsix_voice *voice = &state->voices[i];

        if (voice->state != SLOPSIX_VOICE_ON) {
            continue;
        }

        switch (mode) {
        case SLOPSIX_REL_NOTE:
            if (voice->note != note) {
                continue;
            }
            break;
        case SLOPSIX_REL_ALL:
            break;
        case SLOPSIX_REL_PEDAL:
            if (voice->held) {
                continue;
            }
            break;
        }

        voice->held = false;

        if (!state->sustain) {
            voice->state = SLOPSIX_VOICE_RELEASED;
        }
    }
}

void slopsix_note_on(slopsix_context ctx, uint8_t note, uint8_t velocity)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    size_t i;

    if (state == NULL || note > 127u) {
        return;
    }

    /* Voice allocation (oldest-note-first stealing) arrives with the
     * voice path; for now the first free voice takes the note. */
    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_FREE) {
            state->voices[i].state = SLOPSIX_VOICE_ON;
            state->voices[i].held = true;
            state->voices[i].note = note;
            state->voices[i].velocity = velocity;
            return;
        }
    }
}

void slopsix_note_off(slopsix_context ctx, uint8_t note)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }

    slopsix_release_voices(state, SLOPSIX_REL_NOTE, note);
}

void slopsix_all_notes_off(slopsix_context ctx)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    /* CC 123 "All Notes Off" semantics: act as if every key were released.
     * Voices held by the sustain pedal keep ringing until pedal-up; this is
     * deliberately weaker than an all-sound-off, which would bypass the
     * pedal state machine entirely. */
    slopsix_release_voices(state, SLOPSIX_REL_ALL, 0);
}

void slopsix_set_sustain(slopsix_context ctx, bool on)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->sustain = on;
    if (!on) {
        slopsix_release_voices(state, SLOPSIX_REL_PEDAL, 0);
    }
}

void slopsix_set_mono_mode(slopsix_context ctx, bool mono)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->mono = mono;
}

void slopsix_set_pitch_bend(slopsix_context ctx, uint16_t bend)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->pitch_bend = bend;
}

void slopsix_set_pitch_bend_range(slopsix_context ctx,
                                  uint8_t range_semitones,
                                  uint8_t step)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    /* The hardware validates both to 0..12 and clears larger values. */
    state->bend_range = range_semitones > 12u ? 0u : range_semitones;
    state->bend_step = step > 12u ? 0u : step;
}

void slopsix_set_master_tune(slopsix_context ctx, int32_t cents_q16)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->master_tune_q16 = cents_q16;
}

void slopsix_set_ctrl_value(slopsix_context ctx,
                            slopsix_ctrl_source src, uint8_t value_0_127)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL || (unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return;
    }
    state->ctrl_value[src] = value_0_127 > 127u ? 127u : value_0_127;
}

void slopsix_set_ctrl_range(slopsix_context ctx,
                            slopsix_ctrl_source src, uint8_t range_0_99)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL || (unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return;
    }
    state->ctrl_range[src] = range_0_99 > 99u ? 99u : range_0_99;
}

void slopsix_set_ctrl_assign(slopsix_context ctx,
                             slopsix_ctrl_source src, unsigned flags)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL || (unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return;
    }
    state->ctrl_assign[src] = flags;
}

void slopsix_set_pan(slopsix_context ctx, int32_t pan_q16)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
#if defined(SLOPSIX_EXTRAS)
    if (pan_q16 < 0) {
        pan_q16 = 0;
    }
    if (pan_q16 > SLOPSIX_Q16_ONE) {
        pan_q16 = SLOPSIX_Q16_ONE;
    }
    state->pan_q16 = pan_q16;
#else
    /* Panning is an extra: without it the output stays mono at centre. */
    (void)pan_q16;
#endif
}

void slopsix_set_master_volume(slopsix_context ctx, unsigned level_0_7)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->master_volume = level_0_7 > 7u ? 7u : level_0_7;
}
