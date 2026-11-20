/*
 * src/dx7s/slopsix_voice.c - the DX7S personality's render call.
 *
 * The DX7S shares the operator core, the algorithm schedule, the EG/level
 * pipeline and the per-voice block renderer with the DX7.  This file owns
 * only the orchestration that differs: the key modes' fixed unison groups
 * (D7S-3) with their provisional detune spread, and, in LFO Multi mode,
 * the sixteen per-voice LFOs on the OCF gate with each voice's own
 * pitch-modification word and amp-mod reduction.  Poly, Mono and Single
 * mode delegate to the Mk1 paths, so a neutral DX7S context matches the
 * DX7 pre-output render bit for bit.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3).
 */
#include <string.h>

#include "slopsix_internal.h"

/* --------------------------------------------------- the unison groups
 *
 * The DX7S's four key modes (research dx7s §7, §18; plan D7S-3): Poly and
 * Mono are the Mk1's own paths, Unison Poly is four fixed four-voice
 * groups and Unison Mono one sixteen-voice group.  The grouping is
 * data-shaped and provisional where the evidence is silent: a group is
 * active from its first key event, `seq` orders the oldest-first steal,
 * and a group with no voice still ON (only release tails) is reused
 * before a sounding group is stolen.  A stolen sounding group keeps its
 * envelopes and only changes pitch, the manual's Forced Damp Off default;
 * a fresh or fully released group takes a full trigger.
 */

static unsigned slopsix_dx7s_group_count(uint8_t key_mode)
{
    return (key_mode == (uint8_t)SLOPSIX_KEY_UNISON_POLY)
               ? SLOPSIX_DX7S_UNISON_GROUPS
               : 1u;
}

static unsigned slopsix_dx7s_group_size(uint8_t key_mode)
{
    return (key_mode == (uint8_t)SLOPSIX_KEY_UNISON_POLY)
               ? SLOPSIX_DX7S_UNISON_POLY_VOICES
               : SLOPSIX_VOICE_COUNT;
}

static unsigned slopsix_dx7s_group_first(unsigned group)
{
    return (group == 0u)
               ? 0u
               : group * SLOPSIX_DX7S_UNISON_POLY_VOICES;
}

/* A group is sounding while any of its voices is still keyed on. */
static int slopsix_dx7s_group_sounding(const slopsix_context_state *state,
    unsigned group)
{
    unsigned first = slopsix_dx7s_group_first(group);
    unsigned last = first + slopsix_dx7s_group_size(state->key_mode);
    unsigned v;

    for (v = first; v < last; v++) {
        if (state->voices[v].state == SLOPSIX_VOICE_ON) {
            return 1;
        }
    }
    return 0;
}

/* The voice's own share of the stack's spread (D7S-10).  The ranks are
 * centred on the group, so the two ends move apart symmetrically as the
 * detune rises; detune 0 (the default) makes every voice identical. */
static int32_t slopsix_dx7s_detune_offset(uint8_t detune, unsigned rank,
    unsigned size)
{
    int32_t centred = (int32_t)(2u * rank) - (int32_t)(size - 1u);

    return centred * (int32_t)detune *
           (int32_t)SLOPSIX_DX7S_UNISON_DETUNE_STEP_WORD;
}

void slopsix_dx7s_clear_groups(slopsix_context_state *state)
{
    memset(state->unison_group_seq, 0, sizeof(state->unison_group_seq));
    memset(state->unison_group_note, 0, sizeof(state->unison_group_note));
    memset(state->unison_group_active, 0,
           sizeof(state->unison_group_active));
}

void slopsix_dx7s_apply_detune(slopsix_context_state *state)
{
    unsigned count;
    unsigned size;
    unsigned g;
    unsigned v;

    if (state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_POLY &&
        state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_MONO) {
        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            state->voices[v].unison_offset = 0;
        }
        return;
    }

    count = slopsix_dx7s_group_count(state->key_mode);
    size = slopsix_dx7s_group_size(state->key_mode);

    for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
        state->voices[v].unison_offset = 0;
    }

    for (g = 0; g < count; g++) {
        unsigned first = slopsix_dx7s_group_first(g);
        unsigned rank;

        if (!state->unison_group_active[g]) {
            continue;
        }
        for (rank = 0; rank < size; rank++) {
            state->voices[first + rank].unison_offset =
                slopsix_dx7s_detune_offset(state->unison_detune, rank,
                    size);
        }
    }
}

/* The true release, matching the poly path's (the shared one is static
 * in the Mk1's file). */
static void slopsix_dx7s_release_voice(slopsix_voice *slot,
    slopsix_voice_engine *engine)
{
    slot->held = false;
    slot->state = SLOPSIX_VOICE_RELEASED;
    slopsix_voice_engine_release(engine);
}

/* One voice of a group takes the new key event.  A continued sounding
 * voice keeps its envelopes and only re-pitches; a fresh or released
 * voice gets the full trigger. */
static void slopsix_dx7s_key_voice(slopsix_context_state *state,
    unsigned index, uint8_t note, uint8_t velocity, int continue_envelope)
{
    slopsix_voice *slot = &state->voices[index];
    slopsix_voice_engine *engine = &state->engines[index];
    int32_t target;

    slot->note = note;
    slot->velocity = velocity;
    slot->transpose = state->patch.transpose;
    target = (int32_t)(slopsix_note_word[
        (uint8_t)slopsix_effective_note(note, slot->transpose) & 127u] &
        ~3u);
    slot->porta_current = target;
    slot->porta_target = target;
    slot->gliss_current = target;

    if (continue_envelope && slot->state != SLOPSIX_VOICE_FREE) {
        slot->held = true;
        slot->state = SLOPSIX_VOICE_ON;
        slot->seq = ++state->voice_seq;
        slopsix_voice_engine_update_frequency(state, slot, engine);
    } else {
        slopsix_voice_engine_note_on(state, slot, engine);
    }
}

/* The group the key event takes: an unused or fully released group with
 * the lowest index first, otherwise the oldest sounding group. */
static unsigned slopsix_dx7s_group_pick(slopsix_context_state *state,
    unsigned count, int *steal)
{
    unsigned g;

    for (g = 0; g < count; g++) {
        if (!state->unison_group_active[g] ||
            !slopsix_dx7s_group_sounding(state, g)) {
            *steal = 0;
            return g;
        }
    }

    {
        unsigned oldest = 0;

        for (g = 1; g < count; g++) {
            if (state->unison_group_seq[g] <
                state->unison_group_seq[oldest]) {
                oldest = g;
            }
        }
        *steal = 1;
        return oldest;
    }
}

void slopsix_dx7s_note_on(slopsix_context_state *state, uint8_t note,
    uint8_t velocity)
{
    unsigned count;
    unsigned size;
    unsigned group;
    unsigned first;
    unsigned rank;
    int steal;
    int continue_envelope;

    if (state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_POLY &&
        state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_MONO) {
        slopsix_dx7_note_on(state, note, velocity);
        return;
    }

    count = slopsix_dx7s_group_count(state->key_mode);
    size = slopsix_dx7s_group_size(state->key_mode);
    group = slopsix_dx7s_group_pick(state, count, &steal);
    first = slopsix_dx7s_group_first(group);

    /* A stolen group that is still sounding continues its envelopes. */
    continue_envelope = steal && slopsix_dx7s_group_sounding(state, group);

    state->unison_group_active[group] = 1u;
    state->unison_group_note[group] = note;
    state->unison_group_seq[group] = ++state->voice_seq;

    for (rank = 0; rank < size; rank++) {
        unsigned index = first + rank;

        state->voices[index].unison_offset =
            slopsix_dx7s_detune_offset(state->unison_detune, rank, size);
        slopsix_dx7s_key_voice(state, index, note, velocity,
            continue_envelope);
    }

    state->porta_last_target = state->voices[first].porta_target;
}

void slopsix_dx7s_note_off(slopsix_context_state *state, uint8_t note)
{
    unsigned count;
    unsigned g;

    if (state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_POLY &&
        state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_MONO) {
        slopsix_dx7_note_off(state, note);
        return;
    }

    count = slopsix_dx7s_group_count(state->key_mode);
    for (g = 0; g < count; g++) {
        unsigned first;
        unsigned rank;
        unsigned size;

        if (!state->unison_group_active[g] ||
            state->unison_group_note[g] != note) {
            continue;
        }
        first = slopsix_dx7s_group_first(g);
        size = slopsix_dx7s_group_size(state->key_mode);
        for (rank = 0; rank < size; rank++) {
            unsigned index = first + rank;
            slopsix_voice *voice = &state->voices[index];

            if (voice->state != SLOPSIX_VOICE_ON || !voice->held ||
                voice->note != note) {
                continue;
            }
            if (state->sustain) {
                voice->held = false;
            } else {
                slopsix_dx7s_release_voice(voice, &state->engines[index]);
            }
        }
    }
}

void slopsix_dx7s_all_notes_off(slopsix_context_state *state)
{
    unsigned count;
    unsigned g;

    if (state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_POLY &&
        state->key_mode != (uint8_t)SLOPSIX_KEY_UNISON_MONO) {
        slopsix_dx7_all_notes_off(state);
        return;
    }

    count = slopsix_dx7s_group_count(state->key_mode);
    for (g = 0; g < count; g++) {
        unsigned first;
        unsigned rank;
        unsigned size;

        if (!state->unison_group_active[g]) {
            continue;
        }
        first = slopsix_dx7s_group_first(g);
        size = slopsix_dx7s_group_size(state->key_mode);
        for (rank = 0; rank < size; rank++) {
            unsigned index = first + rank;
            slopsix_voice *voice = &state->voices[index];

            /* CC 123 semantics: the pedal defers the release, exactly as
             * the Mk1's poly all-notes-off does. */
            if (voice->state != SLOPSIX_VOICE_ON || !voice->held) {
                continue;
            }
            if (state->sustain) {
                voice->held = false;
            } else {
                slopsix_dx7s_release_voice(voice, &state->engines[index]);
            }
        }
    }

    /* All-notes-off clears the groups; the pedal-held voices, if any,
     * release on pedal-up through the shared path. */
    slopsix_dx7s_clear_groups(state);
}

/*
 * The personality's own defaults.
 *
 * The DX7S adds parameters the Mk1 has not, and they keep the Mk1's
 * behaviour until the host changes them: Poly key mode, no unison
 * detune, the context-wide LFO in Single mode, and the Mk1's own
 * pitch-EG span (eight octaves) with the velocity switch off and the
 * rate scaling at zero (plan D7S-8).  These are the neutral values a
 * DX7S context must reproduce the Mk1's pre-output render with.
 *
 * The pitch envelope's law is the DX7S's own composition (D7S-4): the
 * shared path resolves it at personality init, exactly as it resolves
 * the output stage and the voice bus, so a DX7 context in this build
 * plays the firmware's envelope.
 */
static void slopsix_dx7s_defaults(slopsix_context_state *state)
{
    state->key_mode = SLOPSIX_KEY_POLY;
    state->unison_detune = 0u;
    state->lfo_mode = SLOPSIX_LFO_SINGLE;
    state->mono = false;
    slopsix_dx7s_clear_groups(state);
    state->patch.peg_range = SLOPSIX_PEG_RANGE_DEFAULT;
    state->patch.peg_velocity = 0u;
    state->patch.peg_rate_scaling = 0u;
    state->pitch_eg_configure = slopsix_dx7s_pitch_eg_configure;
    state->pitch_eg_offset = slopsix_dx7s_pitch_eg_offset;
}

void slopsix_dx7s_init(slopsix_context_state *state, uint32_t sample_rate)
{
    slopsix_dx7_init(state, sample_rate);
    /* The DX7S brings its own tail: re-design the filter from the
     * personality's prototype after the shared DX7 initialization. */
    slopsix_dx7s_filter_init(&state->filter, state->sample_rate);
    /* No compander in the DX7S path: the voices sum at the operator
     * core's resolution, and the 16-bit DAC step saturates the bus. */
    state->voice_bus = slopsix_dx7s_voice_bus;
    state->output_mix = slopsix_dx7s_output_mix;
    /* The personality's own parameters and its pitch-EG law. */
    slopsix_dx7s_defaults(state);
}

void slopsix_dx7s_reset(slopsix_context_state *state)
{
    slopsix_dx7_reset(state);
    slopsix_dx7s_filter_init(&state->filter, state->sample_rate);
    state->voice_bus = slopsix_dx7s_voice_bus;
    state->output_mix = slopsix_dx7s_output_mix;
    slopsix_dx7s_defaults(state);
}

void slopsix_dx7s_set_sample_rate(slopsix_context_state *state,
    uint32_t sample_rate)
{
    slopsix_dx7_set_sample_rate(state, sample_rate);
    slopsix_dx7s_filter_init(&state->filter, state->sample_rate);
}

void slopsix_dx7s_process(slopsix_context ctx, int16_t *stereo_out,
    size_t frames)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    const slopsix_algorithm *alg;
    uint32_t lfo_threshold;
    uint32_t peg_threshold;
    size_t done = 0;
    size_t i;

    if (stereo_out == NULL) {
        return;
    }
    if (state == NULL) {
        for (i = 0; i < frames * 2u; i++) {
            stereo_out[i] = 0;
        }
        return;
    }

    alg = slopsix_algorithm_get(state->patch.algorithm);
    lfo_threshold = slopsix_cadence_threshold(
        state->lfo_cadence.sample_rate);
    peg_threshold = slopsix_cadence_threshold(
        state->pitch_cadence.sample_rate);

    while (done < frames) {
        uint32_t lfo_distance = slopsix_cadence_distance(
            &state->lfo_cadence, lfo_threshold);
        uint32_t peg_distance = slopsix_cadence_distance(
            &state->pitch_cadence, peg_threshold);
        uint32_t tick = (lfo_distance < peg_distance) ? lfo_distance
                                                      : peg_distance;
        size_t left = frames - done;
        size_t plain;

        if (tick == UINT32_MAX || tick - 1u > left) {
            plain = left;
        } else {
            plain = (size_t)(tick - 1u);
        }

        if (plain > 0) {
            slopsix_cadence_skip(&state->lfo_cadence, plain);
            slopsix_cadence_skip(&state->pitch_cadence, plain);
            slopsix_render_span(state, alg, stereo_out, done, plain);
            done += plain;
            if (done == frames) {
                break;
            }
        }

        {
            int lfo_tick = slopsix_cadence_step(&state->lfo_cadence,
                                                lfo_threshold);
            int peg_tick = slopsix_cadence_step(&state->pitch_cadence,
                                                peg_threshold);
            uint16_t porta_changed = 0;

            if (lfo_tick) {
                if (state->lfo_mode == (uint8_t)SLOPSIX_LFO_MULTI) {
                    slopsix_dx7s_lfo_tick(state);
                } else {
                    slopsix_lfo_tick(state);
                }
                porta_changed = slopsix_porta_process(state,
                    state->porta_bank);
                state->porta_bank ^= 1u;
            }
            if (lfo_tick || peg_tick) {
                int multi = state->lfo_mode == (uint8_t)SLOPSIX_LFO_MULTI;
                int32_t single_lfo_offset = 0;
                unsigned v;

                if (!multi) {
                    single_lfo_offset = slopsix_mod_pitch_offset(state);
                }

                for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
                    slopsix_voice_engine *engine = &state->engines[v];
                    int32_t offset;

                    if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                        continue;
                    }
                    if (lfo_tick) {
                        slopsix_voice_engine_update_ams(state, engine);
                    }
                    if (peg_tick) {
                        slopsix_pitch_eg_update(&engine->pitch_eg);
                    }
                    if (multi) {
                        int16_t word = slopsix_mod_pitch_word_one(state,
                            &engine->lfo);
                        offset = slopsix_floor_div((int32_t)word,
                            SLOPSIX_PITCH_MOD_TO_LOG_SHIFT) +
                            state->pitch_eg_offset(&engine->pitch_eg);
                    } else {
                        offset = single_lfo_offset +
                            state->pitch_eg_offset(&engine->pitch_eg);
                    }
                    if (((porta_changed >> v) & 1u) != 0u ||
                        offset != engine->pitch_offset) {
                        slopsix_voice_engine_update_frequency(state,
                            &state->voices[v], engine);
                    }
                }
            }
        }

        slopsix_render_span(state, alg, stereo_out, done, 1);
        done += 1;
    }
}
