/*
 * src/dx7s/slopsix_voice.c - the DX7S personality's render call.
 *
 * The DX7S shares the operator core, the algorithm schedule, the EG/level
 * pipeline and the per-voice block renderer with the DX7.  This file owns
 * only the orchestration that differs: in LFO Multi mode it advances the
 * sixteen per-voice LFOs on the OCF gate and composes each voice's own
 * pitch-modification word and amp-mod reduction.  In Single mode it uses
 * the context-wide Mk1 LFO path, so a neutral DX7S context matches the
 * DX7 pre-output render bit for bit.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3).
 */
#include "slopsix_internal.h"

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
}

void slopsix_dx7s_reset(slopsix_context_state *state)
{
    slopsix_dx7_reset(state);
    slopsix_dx7s_filter_init(&state->filter, state->sample_rate);
    state->voice_bus = slopsix_dx7s_voice_bus;
    state->output_mix = slopsix_dx7s_output_mix;
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
                            slopsix_pitch_eg_offset(&engine->pitch_eg);
                    } else {
                        offset = single_lfo_offset +
                            slopsix_pitch_eg_offset(&engine->pitch_eg);
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
