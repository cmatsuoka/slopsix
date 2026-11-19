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

static void slopsix_dx7_defaults(slopsix_context_state *state)
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
    state->master_gain_q16 = slopsix_master_gain_q16(state->master_volume);
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence,
        state->sample_rate);
    slopsix_lfo_cadence_init(&state->lfo_cadence, state->sample_rate);
    /* The analog tail's filter: the design follows the host's rate and
     * its state starts cleared. */
    slopsix_filter_init(&state->filter, state->sample_rate);
    /* The controller arithmetic's cached values: the memset leaves all
     * inputs, ranges and routings at zero, and nothing routed is the
     * neutral total - the routine still runs once, as the firmware's
     * reset path does. */
    slopsix_mod_process_input_sources(state);
}

void slopsix_dx7_init(slopsix_context_state *state, uint32_t sample_rate)
{
    slopsix_dx7_defaults(state);
    if (sample_rate != 0u) {
        state->sample_rate = sample_rate;
        slopsix_pitch_eg_cadence_init(&state->pitch_cadence, sample_rate);
        slopsix_lfo_cadence_init(&state->lfo_cadence, sample_rate);
        slopsix_filter_init(&state->filter, sample_rate);
    }
}

void slopsix_dx7_reset(slopsix_context_state *state)
{
    uint32_t rate = state->sample_rate;

    slopsix_dx7_defaults(state);
    state->sample_rate = rate;
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence, rate);
    slopsix_lfo_cadence_init(&state->lfo_cadence, rate);
    slopsix_filter_init(&state->filter, rate);
}

void slopsix_dx7_set_sample_rate(slopsix_context_state *state,
    uint32_t sample_rate)
{
    unsigned i;
    unsigned k;

    if (sample_rate == 0u) {
        return;
    }
    state->sample_rate = sample_rate;
    slopsix_pitch_eg_cadence_init(&state->pitch_cadence, sample_rate);
    slopsix_lfo_cadence_init(&state->lfo_cadence, sample_rate);
    /* The analog tail's filter follows the host's rate: its digital
     * design is derived again, and its state starts cleared. */
    slopsix_filter_init(&state->filter, sample_rate);

    /* The sounding voices re-anchor: the envelopes take the host rate
     * (a stage already in progress keeps its current timing until the
     * next stage change), and the phase increments are recomposed from
     * the voices' current pitch state. */
    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_FREE) {
            continue;
        }
        for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
            state->engines[i].eg[k].sample_rate = sample_rate;
        }
        slopsix_voice_engine_update_frequency(state, &state->voices[i],
            &state->engines[i]);
    }
}

/* ---------------------------------------------------- the shared glue */

slopsix_context slopsix_create_context(slopsix_engine engine,
    uint32_t sample_rate)
{
    slopsix_context_impl *impl;

    switch (engine) {
    case SLOPSIX_ENGINE_DX7:
        break;
    case SLOPSIX_ENGINE_DX100:
        /* The DX100 personality is planned but is not part of any build
         * yet: its translation units arrive with the DX100 phase.  The
         * request is rejected here, predictably, rather than producing a
         * context that could not render (library page §0.2's rule: an
         * absent personality is rejected at creation). */
        return NULL;
    default:
        return NULL;
    }

    impl = (slopsix_context_impl *)calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return NULL;
    }

    impl->engine = engine;
    switch (engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_init(&impl->dx7, sample_rate);
        impl->sample_rate = impl->dx7.sample_rate;
        impl->process = slopsix_dx7_process;
        break;
    default:
        free(impl);
        return NULL;
    }

    return (slopsix_context)impl;
}

void slopsix_destroy_context(slopsix_context ctx)
{
    free(ctx);
}

void slopsix_set_sample_rate(slopsix_context ctx, uint32_t sample_rate)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL || sample_rate == 0u) {
        return;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_sample_rate(&impl->dx7, sample_rate);
        impl->sample_rate = impl->dx7.sample_rate;
        break;
    default:
        break;
    }
}

void slopsix_reset(slopsix_context ctx)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL) {
        return;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_reset(&impl->dx7);
        break;
    default:
        break;
    }
}

/* The per-sample render lives in src/slopsix_voice.c (the per-sample
 * path is compiled speed-first); this file owns the context, the patch
 * and the note events. */

/*
 * The render call: the personality's entry point was resolved at
 * creation, so this is one indirect call per buffer and the sample loop
 * carries no personality branch (plan decision D-18).
 */
void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    size_t i;

    if (impl == NULL || impl->process == NULL) {
        /* The render call always writes deterministic output, even on a
         * null context. */
        if (stereo_out != NULL) {
            for (i = 0; i < frames * 2u; i++) {
                stereo_out[i] = 0;
            }
        }
        return;
    }

    impl->process(ctx, stereo_out, frames);
}

static void slopsix_patch_parse(slopsix_context_state *state);

/* PATCH_ACTIVATE opens with VOICE_RESET_EGS, which silences every EGS
 * voice before the new registers are loaded (research fw §5).  The
 * firmware leaves its CPU-side key-event buffer alone, so a key that is
 * still down keeps its voice allocated; it simply stops sounding. */
static void slopsix_patch_silence_voices(slopsix_context_state *state)
{
    unsigned i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state != SLOPSIX_VOICE_FREE) {
            slopsix_voice_engine_silence(&state->engines[i]);
        }
    }
}

slopsix_error slopsix_set_voice(slopsix_context ctx, const uint8_t *data,
    size_t size)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL || data == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        /* The dump's size is the engine's own; a mismatch is a host
         * error and the dump is not loaded (library page §3). */
        if (size != SLOPSIX_VOICE_DUMP_SIZE) {
            return SLOPSIX_ERR_RANGE;
        }
        memcpy(impl->dx7.voice_dump, data, SLOPSIX_VOICE_DUMP_SIZE);
        slopsix_patch_silence_voices(&impl->dx7);
        slopsix_patch_parse(&impl->dx7);
        return SLOPSIX_OK;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }
}

slopsix_error slopsix_get_voice(const slopsix_context ctx, uint8_t *out,
    size_t size)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;

    if (impl == NULL || out == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        if (size != SLOPSIX_VOICE_DUMP_SIZE) {
            return SLOPSIX_ERR_RANGE;
        }
        memcpy(out, impl->dx7.voice_dump, SLOPSIX_VOICE_DUMP_SIZE);
        return SLOPSIX_OK;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }
}

/* ------------------------------------------------------- patch activation */

/* Clamp a dump byte onto the 0..99 parameter range. */
static uint8_t slopsix_param99(uint8_t value)
{
    return value > 99u ? 99u : value;
}

/*
 * Patch activation: parse the dump into the live patch.
 *
 * The firmware's own activation (PATCH_ACTIVATE and its per-operator
 * calls) walks the six operator blocks and ships each one's values to
 * the EGS; the keyboard level scaling's 6 x 43 curve is built once per
 * patch into RAM at the same point (research E-05).  The engine parses
 * the block into the patch state and builds the KLS curve here, so a
 * note-on composes the per-voice values from a ready patch.
 *
 * The out-of-range dump bytes are clamped onto their parameter's range;
 * the coarse/fine/detune fields need no clamp here because the
 * frequency path clips them to the hardware's field widths already.
 */
static void slopsix_patch_parse(slopsix_context_state *state)
{
    const uint8_t *dump = state->voice_dump;
    unsigned block;
    unsigned i;

    for (block = 0; block < SLOPSIX_OP_COUNT; block++) {
        /* Block order: operator 6 first, operator 1 last; the engine's
         * op index runs operator 1 first. */
        const uint8_t *src =
            dump + (unsigned)block * SLOPSIX_DUMP_OP_BLOCK;
        slopsix_patch_op *op =
            &state->patch.ops[SLOPSIX_OP_COUNT - 1u - block];

        for (i = 0; i < 4u; i++) {
            op->rates[i] = slopsix_param99(src[i]);
            op->levels[i] = slopsix_param99(src[4u + i]);
        }
        op->rate_scaling = src[13] & 7u;
        op->ams_sens = src[14] & 3u;
        op->velocity_sens = src[15] & 7u;
        op->out_level = slopsix_param99(src[16]);
        op->mode = src[17] != 0u ? 1u : 0u;
        op->coarse = src[18];
        op->fine = src[19];
        op->detune = src[20] > 14u ? 14u : src[20];

        /* The keyboard level scaling curve; the depths and the two
         * curve fields are the hardware's own 0..99 resp. 0..3 fields. */
        slopsix_kls_build(op->kls, src[8], src[9], src[10],
            src[11] & 3u, src[12] & 3u);
    }

    /* The algorithm field goes to the OPS as the firmware sends it: the
     * listing's PATCH_ACTIVATE_ALG_MODE composes the $2805 register as
     * `PATCH_ALGORITHM << 3 | feedback` with no adjustment, and the
     * field spans the ROM's 0..31 rows (0-based; the panel displays one
     * more).  Out-of-range bytes wrap, as three bits of the register
     * would. */
    {
        state->patch.algorithm = (uint8_t)(dump[SLOPSIX_DUMP_ALGORITHM] & 31u);
    }
    state->patch.feedback = dump[SLOPSIX_DUMP_FEEDBACK] & 7u;
    state->patch.osc_sync = dump[SLOPSIX_DUMP_OSC_SYNC] != 0u;

    /* The transpose byte counts from 24 (its neutral value); the dump's
     * 0..48 byte covers -24..+24 semitones and anything outside it is
     * clamped. */
    {
        int32_t transpose = (int32_t)dump[SLOPSIX_DUMP_TRANSPOSE] - 24;

        if (transpose > 24) {
            transpose = 24;
        }
        state->patch.transpose = (int8_t)transpose;
    }

    /* The pitch EG's rate/level pairs and the LFO block's activation:
     * the pitch EG parses its four pairs; the LFO's bytes go through the
     * firmware's own activation scaling into the context's LFO state,
     * whose parameters are per-patch exactly as the hardware's
     * registers are. */
    for (i = 0; i < 4u; i++) {
        state->patch.peg_rates[i] =
            slopsix_param99(dump[SLOPSIX_DUMP_PEG_RATE + i]);
        state->patch.peg_levels[i] =
            slopsix_param99(dump[SLOPSIX_DUMP_PEG_LEVEL + i]);
    }
    slopsix_lfo_patch_activate(state, dump);
}

/* ------------------------------------------------------- voice management
 *
 * The voices follow the firmware's own bookkeeping (research fw
 * VOICE_ADD / VOICE_REMOVE and their mono paths):
 *
 *   - Polyphonic: a note-on takes an inactive voice, oldest first when
 *     all sixteen are in use (the library page §4's allocation rule).
 *     A key release reaches only the voices whose key is still down; a
 *     voice the sustain pedal holds is marked and keeps ringing until
 *     pedal-up, exactly as the firmware's VOICE_STATUS bit does.
 *   - Monophonic: one sounding voice, always voices[0].  The first key
 *     after silence is a full trigger (pitch EG reset, envelopes out of
 *     silence, velocity applied).  An overlapping key press is legato:
 *     the voice re-pitches in place and no envelope is touched, with
 *     the legato direction deciding which remaining key sounds when
 *     one is released.  The sustain pedal defers only the final
 *     release; the key events it holds keep leaving the list.
 *
 * In both modes a true release - the one that actually sends the key
 * off - runs the engine's release once: the operator envelopes' key-up
 * jump to the release stage and the pitch EG takes its release step.
 */

/* A voice's true release: the key-off event. */
static void slopsix_release_voice(slopsix_voice *slot,
    slopsix_voice_engine *engine)
{
    slot->held = false;
    slot->state = SLOPSIX_VOICE_RELEASED;
    slopsix_voice_engine_release(engine);
}

/* Polyphonic key release: every still-held voice of that note. */
static void slopsix_poly_release_note(slopsix_context_state *state,
    uint8_t note)
{
    unsigned i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        slopsix_voice *voice = &state->voices[i];

        if (voice->state != SLOPSIX_VOICE_ON || !voice->held ||
            voice->note != note) {
            continue;
        }

        if (state->sustain) {
            /* The pedal holds the note: mark it and keep ringing. */
            voice->held = false;
        } else {
            slopsix_release_voice(voice, &state->engines[i]);
        }
    }
}

/* Polyphonic all-notes-off / pedal-up: every matching voice releases. */
static void slopsix_poly_release_all(slopsix_context_state *state,
    bool only_held)
{
    unsigned i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        slopsix_voice *voice = &state->voices[i];

        if (voice->state != SLOPSIX_VOICE_ON || voice->held != only_held) {
            continue;
        }

        /* The pedal defers a held key exactly as a single release does. */
        if (state->sustain && only_held) {
            voice->held = false;
        } else {
            slopsix_release_voice(voice, &state->engines[i]);
        }
    }
}

/* The voice allocation: the first free voice, else the oldest one. */
static unsigned slopsix_poly_pick(slopsix_context_state *state)
{
    unsigned i;
    unsigned oldest = 0;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_FREE) {
            return i;
        }
    }

    for (i = 1; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].seq < state->voices[oldest].seq) {
            oldest = i;
        }
    }
    return oldest;
}

/* Mono mode's active-key list: a key event in, a slot found. */
static int slopsix_mono_find_key(const slopsix_context_state *state,
    uint8_t note)
{
    int i;

    for (i = 0; i < state->mono_count; i++) {
        if (state->mono_keys[i] == note) {
            return i;
        }
    }
    return -1;
}

static void slopsix_mono_remove_key(slopsix_context_state *state, int index)
{
    int i;

    if (index < 0 || index >= state->mono_count) {
        return;
    }
    for (i = index; i < state->mono_count - 1; i++) {
        state->mono_keys[i] = state->mono_keys[i + 1];
    }
    state->mono_keys[state->mono_count - 1] = 0;
    state->mono_count--;
}

/* Mono key-on: the full trigger, then the legato overlap. */
static void slopsix_mono_note_on(slopsix_context_state *state,
    uint8_t note, uint8_t velocity)
{
    slopsix_voice *slot = &state->voices[0];
    int i;

    if (state->mono_count >= SLOPSIX_VOICE_COUNT) {
        /* The firmware's key-event buffer is full and takes no more. */
        return;
    }

    /* The event joins the list whatever the legato decides. */
    for (i = state->mono_count; i < SLOPSIX_VOICE_COUNT; i++) {
        state->mono_keys[i] = 0;
    }
    state->mono_keys[state->mono_count++] = note;

    if (state->mono_count == 1) {
        /* The first key after silence: a full trigger, with the pitch
         * EG reset and the velocity applied (VOICE_ADD_MONO's count-1
         * path: key off, pitch EG reset, operator data, key on). */
        slot->note = note;
        slot->velocity = velocity;
        slopsix_voice_engine_note_on(state, slot, &state->engines[0]);
        state->mono_last_key = note;
        return;
    }

    /* The legato direction: the second key sets it, later keys extend
     * it.  A press that does not extend the direction leaves last key
     * and sounding pitch untouched - the firmware's M_LEGATO_DIRECTION
     * rule. */
    if (state->mono_count == 2) {
        state->mono_dir = (note >= state->mono_last_key) ? 1u : 0u;
        state->mono_last_key = note;
    } else if (state->mono_dir) {
        if (note >= state->mono_last_key) {
            state->mono_last_key = note;
        }
    } else {
        if (note <= state->mono_last_key) {
            state->mono_last_key = note;
        }
    }

    /* The re-pitch: no envelope is touched and the old velocity stays. */
    slot->note = state->mono_last_key;
    slopsix_voice_engine_update_frequency(state, slot, &state->engines[0]);
}

/* Mono key-off: the release, or the legato fallback re-pitch. */
static void slopsix_mono_note_off(slopsix_context_state *state, uint8_t note)
{
    slopsix_voice *slot = &state->voices[0];
    int index = slopsix_mono_find_key(state, note);
    int i;

    if (index < 0) {
        return;
    }
    slopsix_mono_remove_key(state, index);

    if (state->mono_count == 0) {
        if (state->sustain) {
            /* The pedal defers the final release. */
            slot->held = false;
        } else {
            slopsix_release_voice(slot, &state->engines[0]);
        }
        return;
    }

    /* The legato fallback: the direction picks the remaining key. */
    {
        uint8_t pick = state->mono_keys[0];

        for (i = 1; i < state->mono_count; i++) {
            if (state->mono_dir) {
                if (state->mono_keys[i] > pick) {
                    pick = state->mono_keys[i];
                }
            } else {
                if (state->mono_keys[i] < pick) {
                    pick = state->mono_keys[i];
                }
            }
        }
        state->mono_last_key = pick;
        slot->note = pick;
        slopsix_voice_engine_update_frequency(state, slot,
            &state->engines[0]);
    }
}

/* --------------------------------------------------- the note events */

/*
 * The DX7's note events.  The poly and mono paths are the firmware's own
 * bookkeeping (see the section above); the public entry points dispatch
 * to them per personality.
 */
static void slopsix_dx7_note_on(slopsix_context_state *state, uint8_t note,
    uint8_t velocity)
{
    if (state->mono) {
        slopsix_mono_note_on(state, note, velocity);
        return;
    }

    {
        unsigned index = slopsix_poly_pick(state);
        slopsix_voice *slot = &state->voices[index];

        slot->note = note;
        slot->velocity = velocity;
        slopsix_voice_engine_note_on(state, slot, &state->engines[index]);
    }
}

static void slopsix_dx7_note_off(slopsix_context_state *state, uint8_t note)
{
    if (state->mono) {
        slopsix_mono_note_off(state, note);
        return;
    }
    slopsix_poly_release_note(state, note);
}

static void slopsix_dx7_all_notes_off(slopsix_context_state *state)
{
    /* CC 123 "All Notes Off" semantics: act as if every key were released.
     * Voices held by the sustain pedal keep ringing until pedal-up; this is
     * deliberately weaker than an all-sound-off, which would bypass the
     * pedal state machine entirely. */
    if (state->mono) {
        int i;

        if (state->mono_count == 0) {
            return;
        }
        for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
            state->mono_keys[i] = 0;
        }
        state->mono_count = 0;
        if (state->sustain) {
            state->voices[0].held = false;
        } else {
            slopsix_release_voice(&state->voices[0], &state->engines[0]);
        }
        return;
    }
    slopsix_poly_release_all(state, true);
}

static void slopsix_dx7_set_sustain(slopsix_context_state *state, bool on)
{
    state->sustain = on;
    if (!on) {
        /* Pedal-up releases every voice the pedal was holding; the keys
         * still down keep sounding. */
        slopsix_poly_release_all(state, false);
    }
}

static void slopsix_dx7_set_mono_mode(slopsix_context_state *state, bool mono)
{
    state->mono = mono;
    if (mono) {
        /* The mode switch clears the key-event bookkeeping; the voices
         * already sounding keep their envelopes, as the EGS slots do. */
        memset(state->mono_keys, 0, sizeof(state->mono_keys));
        state->mono_count = 0;
        state->mono_dir = 0u;
        state->mono_last_key = 0u;
    }
}

/* ------------------------------------------------ the parameter setters */

static void slopsix_dx7_set_pitch_bend(slopsix_context_state *state,
    uint16_t bend)
{
    state->pitch_bend = bend;
    /* The bend is part of the pitch-modification register's word: the
     * context recomposes it so the change is audible without waiting for
     * the next LFO tick. */
    state->lfo.pitch_mod_word = slopsix_mod_pitch_word(state);
    slopsix_refresh_pitch(state);
}

static void slopsix_dx7_set_pitch_bend_range(slopsix_context_state *state,
    uint8_t range_semitones,
    uint8_t step)
{
    /* The hardware validates the range to 0..12 and clears a larger one
     * (P-05: the power-up check and the panel max table).  The step is a
     * 4-bit register exposed as it is (plan D-3): the bend parse reads
     * the step table's sixteen groups, and the zero groups 13..15 make
     * the bending inert - the hardware's own validation would clear
     * them, so a panel path never leaves them in the register. */
    state->bend_range = range_semitones > 12u ? 0u : range_semitones;
    state->bend_step = step > 15u ? 0u : step;
    state->lfo.pitch_mod_word = slopsix_mod_pitch_word(state);
    slopsix_refresh_pitch(state);
}

static void slopsix_dx7_set_master_tune(slopsix_context_state *state,
    int32_t cents_q16)
{
    state->master_tune_q16 = cents_q16;
    slopsix_refresh_pitch(state);
}

/*
 * The recompose the three controller setters share (the firmware recomputes
 * MOD_PROCESS_INPUT_SOURCES on every controller event): the scaled inputs
 * and the two factors, then the pitch-modification word with the term
 * recomposed from the last tick's wave amplitude, so a controller change
 * to the pitch register is audible without waiting for the next LFO tick.
 * The amp-mod register itself follows on the next tick, as the firmware's
 * per-tick MOD_AMP_LOAD_TO_EGS pushes it.
 */
static void slopsix_mod_refresh(slopsix_context_state *state)
{
    slopsix_mod_process_input_sources(state);
    slopsix_mod_pitch_term_update(state);
    state->lfo.pitch_mod_word = slopsix_mod_pitch_word(state);
}

static void slopsix_dx7_set_ctrl_value(slopsix_context_state *state,
    slopsix_ctrl_source src,
    uint8_t value_0_127)
{
    state->ctrl_value[src] = value_0_127 > 127u ? 127u : value_0_127;
    slopsix_mod_refresh(state);
}

static void slopsix_dx7_set_ctrl_range(slopsix_context_state *state,
    slopsix_ctrl_source src,
    uint8_t range_0_99)
{
    state->ctrl_range[src] = range_0_99 > 99u ? 99u : range_0_99;
    slopsix_mod_refresh(state);
}

static void slopsix_dx7_set_ctrl_assign(slopsix_context_state *state,
    slopsix_ctrl_source src,
    unsigned flags)
{
    /* The hardware's routing byte holds the three bits: pitch,
     * amplitude and EG bias (the firmware's M_MOD_WHEEL_ASSIGN_FLAGS). */
    state->ctrl_assign[src] = flags & (SLOPSIX_ASSIGN_PITCH |
                                       SLOPSIX_ASSIGN_AMP |
                                       SLOPSIX_ASSIGN_EG);
    slopsix_mod_refresh(state);
}

#if defined(SLOPSIX_EXTRAS)
/* The pan is an extra, so the routine exists only in the extras build:
 * without it the output stays mono at centre and the public call reports
 * the feature as absent. */
static void slopsix_dx7_set_pan(slopsix_context_state *state, int32_t pan_q16)
{
    if (pan_q16 < 0) {
        pan_q16 = 0;
    }
    if (pan_q16 > SLOPSIX_Q16_ONE) {
        pan_q16 = SLOPSIX_Q16_ONE;
    }
    state->pan_q16 = pan_q16;
}
#endif

static void slopsix_dx7_set_master_volume(slopsix_context_state *state,
    unsigned level_0_7)
{
    state->master_volume = level_0_7 > 7u ? 7u : level_0_7;
    state->master_gain_q16 = slopsix_master_gain_q16(state->master_volume);
}

/* ------------------------------------------------------- the public API */

/*
 * The public setters and note events.  Each one resolves the context's
 * personality and reports what it did:
 *
 *   SLOPSIX_OK             the request was applied as it was given
 *   SLOPSIX_ERR_RANGE      a value was out of range and was clamped
 *   SLOPSIX_ERR_UNSUPPORTED the engine does not have the feature; the
 *                           value is left unchanged
 *   SLOPSIX_ERR_INVALID    the call itself was malformed
 */
slopsix_error slopsix_note_on(slopsix_context ctx, uint8_t note,
    uint8_t velocity)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if (note > 127u) {
        note = 127u;
        err = SLOPSIX_ERR_RANGE;
    }
    if (velocity > 127u) {
        velocity = 127u;
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_note_on(&impl->dx7, note, velocity);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_note_off(slopsix_context ctx, uint8_t note)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if (note > 127u) {
        note = 127u;
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_note_off(&impl->dx7, note);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_all_notes_off(slopsix_context ctx)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_all_notes_off(&impl->dx7);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return SLOPSIX_OK;
}

slopsix_error slopsix_set_sustain(slopsix_context ctx, bool on)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_sustain(&impl->dx7, on);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return SLOPSIX_OK;
}

slopsix_error slopsix_set_mono_mode(slopsix_context ctx, bool mono)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_mono_mode(&impl->dx7, mono);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return SLOPSIX_OK;
}

slopsix_error slopsix_set_pitch_bend(slopsix_context ctx, uint16_t bend)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if (bend > 0x3FFFu) {
        bend = 0x3FFFu;
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_pitch_bend(&impl->dx7, bend);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_set_pitch_bend_range(slopsix_context ctx,
    uint8_t range_semitones,
    uint8_t step)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if (range_semitones > 12u || step > 15u) {
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_pitch_bend_range(&impl->dx7, range_semitones, step);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_set_master_tune(slopsix_context ctx, int32_t cents_q16)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_master_tune(&impl->dx7, cents_q16);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return SLOPSIX_OK;
}

slopsix_error slopsix_set_ctrl_value(slopsix_context ctx,
    slopsix_ctrl_source src,
    uint8_t value_0_127)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if ((unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return SLOPSIX_ERR_INVALID;
    }
    if (value_0_127 > 127u) {
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_ctrl_value(&impl->dx7, src, value_0_127);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_set_ctrl_range(slopsix_context ctx,
    slopsix_ctrl_source src,
    uint8_t range_0_99)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if ((unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return SLOPSIX_ERR_INVALID;
    }
    if (range_0_99 > 99u) {
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_ctrl_range(&impl->dx7, src, range_0_99);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_set_ctrl_assign(slopsix_context ctx,
    slopsix_ctrl_source src,
    unsigned flags)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    unsigned known = SLOPSIX_ASSIGN_PITCH | SLOPSIX_ASSIGN_AMP |
                     SLOPSIX_ASSIGN_EG;
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if ((unsigned)src >= SLOPSIX_CTRL_SRC_COUNT) {
        return SLOPSIX_ERR_INVALID;
    }
    if ((flags & ~known) != 0u) {
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_ctrl_assign(&impl->dx7, src, flags);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

slopsix_error slopsix_set_pan(slopsix_context ctx, int32_t pan_q16)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
#if defined(SLOPSIX_EXTRAS)
    slopsix_error err = SLOPSIX_OK;
#endif

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }

#if !defined(SLOPSIX_EXTRAS)
    /* Panning is an extra: without the macro the feature is absent, the
     * output stays mono at centre and the call says so. */
    (void)pan_q16;
    return SLOPSIX_ERR_UNSUPPORTED;
#else
    if (pan_q16 < 0) {
        pan_q16 = 0;
        err = SLOPSIX_ERR_RANGE;
    }
    if (pan_q16 > SLOPSIX_Q16_ONE) {
        pan_q16 = SLOPSIX_Q16_ONE;
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_pan(&impl->dx7, pan_q16);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
#endif
}

slopsix_error slopsix_set_master_volume(slopsix_context ctx,
    unsigned level_0_7)
{
    slopsix_context_impl *impl = SLOPSIX_IMPL(ctx);
    slopsix_error err = SLOPSIX_OK;

    if (impl == NULL) {
        return SLOPSIX_ERR_INVALID;
    }
    if (level_0_7 > 7u) {
        err = SLOPSIX_ERR_RANGE;
    }

    switch (impl->engine) {
    case SLOPSIX_ENGINE_DX7:
        slopsix_dx7_set_master_volume(&impl->dx7, level_0_7);
        break;
    default:
        return SLOPSIX_ERR_UNSUPPORTED;
    }

    return err;
}

/* ------------------------------------------------------- the capabilities */

/* The engine's shape: the numbers a host needs to write engine-agnostic
 * code (library page §0.2). */
static void slopsix_engine_shape(slopsix_engine engine, size_t *voices,
    size_t *operators, size_t *algorithms, size_t *dump_size)
{
    *voices = 0u;
    *operators = 0u;
    *algorithms = 0u;
    *dump_size = 0u;

    switch (engine) {
    case SLOPSIX_ENGINE_DX7:
        *voices = SLOPSIX_VOICE_COUNT;
        *operators = SLOPSIX_OP_COUNT;
        *algorithms = SLOPSIX_ALGORITHM_COUNT;
        *dump_size = SLOPSIX_VOICE_DUMP_SIZE;
        break;
    default:
        break;
    }
}

slopsix_engine slopsix_context_engine(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;

    if (impl == NULL) {
        return SLOPSIX_ENGINE_COUNT;
    }
    return impl->engine;
}

size_t slopsix_voice_count(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;
    size_t voices;
    size_t operators;
    size_t algorithms;
    size_t dump_size;

    if (impl == NULL) {
        return 0u;
    }
    slopsix_engine_shape(impl->engine, &voices, &operators, &algorithms,
        &dump_size);
    return voices;
}

size_t slopsix_operator_count(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;
    size_t voices;
    size_t operators;
    size_t algorithms;
    size_t dump_size;

    if (impl == NULL) {
        return 0u;
    }
    slopsix_engine_shape(impl->engine, &voices, &operators, &algorithms,
        &dump_size);
    return operators;
}

size_t slopsix_algorithm_count(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;
    size_t voices;
    size_t operators;
    size_t algorithms;
    size_t dump_size;

    if (impl == NULL) {
        return 0u;
    }
    slopsix_engine_shape(impl->engine, &voices, &operators, &algorithms,
        &dump_size);
    return algorithms;
}

size_t slopsix_voice_dump_size(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;
    size_t voices;
    size_t operators;
    size_t algorithms;
    size_t dump_size;

    if (impl == NULL) {
        return 0u;
    }
    slopsix_engine_shape(impl->engine, &voices, &operators, &algorithms,
        &dump_size);
    return dump_size;
}

uint32_t slopsix_context_sample_rate(const slopsix_context ctx)
{
    const slopsix_context_impl *impl = (const slopsix_context_impl *)ctx;

    if (impl == NULL) {
        return 0u;
    }
    return impl->sample_rate;
}

uint32_t slopsix_version_number(void)
{
    return SLOPSIX_VERSION_NUMBER;
}
