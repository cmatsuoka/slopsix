/*
 * slopsix.c - context life cycle, public API entry points.
 *
 * The audio path proper lives in the per-sample translation units
 * (slopsix_operator.c, slopsix_eg.c, slopsix_pitch.c, slopsix_output.c);
 * this file owns the context, the parameter state and the public entry
 * points.  Nothing here allocates after the context exists.
 */
#include <stdlib.h>
#include <string.h>

#include "slopsix_internal.h"

/* The generated tables (src/slopsix_tables.c) are shared const data: no
 * per-context copies (plan decision D-4). */
extern const int16_t slopsix_logsin_quarter[1024];
extern const uint16_t slopsix_exp_q11[1024];
extern const uint8_t slopsix_note_t[128];
extern const uint16_t slopsix_note_word[128];
extern const uint8_t slopsix_com_log[6];
extern const int32_t slopsix_coarse_offset[32];
extern const int16_t slopsix_fine_offset[100];

/* Keep the generated tables referenced so the compiler cannot drop them
 * before the render path uses them. */
static void slopsix_tables_keepalive(void)
{
    (void)slopsix_logsin_quarter;
    (void)slopsix_exp_q11;
    (void)slopsix_note_t;
    (void)slopsix_note_word;
    (void)slopsix_com_log;
    (void)slopsix_coarse_offset;
    (void)slopsix_fine_offset;
}

static void slopsix_context_defaults(slopsix_context_state *state)
{
    memset(state, 0, sizeof(*state));
    state->sample_rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    state->pitch_bend = 0x2000u;
    state->bend_range = 12u;
    state->bend_step = 0u;
    state->master_tune_q16 = 0;
    state->pan_q16 = SLOPSIX_Q16_ONE / 2;
    state->master_volume = 4u;
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
    }
    slopsix_tables_keepalive();

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
}

void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames)
{
    size_t i;

    if (stereo_out == NULL) {
        return;
    }
    if (ctx == NULL) {
        /* No context: the buffer is still filled, with silence, so that
         * the render call always writes deterministic output. */
        for (i = 0; i < frames * 2u; i++) {
            stereo_out[i] = 0;
        }
        return;
    }

    /* The voice path arrives in Phase 3; until then the output is
     * silence. */
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
    size_t i;

    if (state == NULL) {
        return;
    }

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_ON &&
            state->voices[i].note == note) {
            state->voices[i].held = false;
            if (!state->sustain) {
                state->voices[i].state = SLOPSIX_VOICE_RELEASED;
            }
        }
    }
}

void slopsix_all_notes_off(slopsix_context ctx)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    size_t i;

    if (state == NULL) {
        return;
    }
    /* CC 123 "All Notes Off" semantics: act as if every key were released.
     * Voices held by the sustain pedal keep ringing until pedal-up; this is
     * deliberately weaker than an all-sound-off, which would bypass the
     * pedal state machine entirely. */
    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_ON) {
            state->voices[i].held = false;
            if (!state->sustain) {
                state->voices[i].state = SLOPSIX_VOICE_RELEASED;
            }
        }
    }
}

void slopsix_set_sustain(slopsix_context ctx, bool on)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    size_t i;

    if (state == NULL) {
        return;
    }
    state->sustain = on;
    if (!on) {
        for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
            if (state->voices[i].state == SLOPSIX_VOICE_ON &&
                !state->voices[i].held) {
                state->voices[i].state = SLOPSIX_VOICE_RELEASED;
            }
        }
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
    if (pan_q16 < 0) {
        pan_q16 = 0;
    }
    if (pan_q16 > SLOPSIX_Q16_ONE) {
        pan_q16 = SLOPSIX_Q16_ONE;
    }
    state->pan_q16 = pan_q16;
}

void slopsix_set_master_volume(slopsix_context ctx, unsigned level_0_7)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

    if (state == NULL) {
        return;
    }
    state->master_volume = level_0_7 > 7u ? 7u : level_0_7;
}
