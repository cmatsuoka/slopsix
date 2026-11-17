/*
 * slopsix_voice.c - The per-sample render path: the voice loop, the
 * pitch EG's cadence and the voice engine's note-on / release / rewrite.
 *
 * The loop follows the hardware's per-sample walk: every voice's
 * operators render in the graph's order, each voice's carrier sum goes
 * through the compander, the quantized voices sum on the bus, and the
 * once-per-sample output stages turn the bus into the two channels:
 *
 *   for each voice:  graph walk -> carrier sum (int32)
 *                    -> compander quantize (12-bit mantissa + shift)
 *   bus sum (wide accumulator, plan decision D-15)
 *   output stages -> int16 L/R
 *
 * The pitch EG and the LFO are CPU-side processes on global cadences
 * (plan decision D-1): the context's gates fire at the firmware's tick
 * rates (the LFO and its register pushes on every OCF tick, the pitch EG
 * on every second tick), advance the respective staircases for the whole
 * instrument, and rewrite the voices' phase increments - the firmware's
 * PORTA_PROCESS does the same rewrite at the same ticks.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
 */
#include "slopsix_internal.h"

/*
 * The voice's frequency rewrite: the effective note (the key plus the
 * patch's transpose), the master tune and the voice's current pitch
 * offset (the bend's contribution and the pitch EG's staircase level)
 * compose into every operator's phase increment.
 *
 * A fixed-frequency operator ignores the pitch offset, as the frequency
 * path states; the voice-floor clamp applies to the composed word in
 * the same place the firmware clamps its own.
 *
 * The note word, pitch offset, floor clamp and master tune are the same
 * for every ratio-mode operator, so they are composed once per voice and
 * each operator then adds only its own ratio and detune.
 */
void slopsix_voice_engine_update_frequency(slopsix_context_state *state,
    const slopsix_voice *slot, slopsix_voice_engine *engine)
{
    /* The pitch-modification register's word (the bend plus the LFO's
     * per-tick wave term) enters the word converted into the engine's
     * 4096-per-octave units, beside the pitch EG's staircase level: the
     * chip adds the register's word to each voice's frequency, while the
     * pitch EG's level is folded into the voice-frequency word itself. */
    int32_t pitch_offset = slopsix_mod_pitch_offset(state) +
        slopsix_pitch_eg_offset(&engine->pitch_eg);
    int32_t note = slopsix_effective_note(slot->note, slot->transpose);
    int32_t voice = slopsix_voice_word((uint8_t)note,
        state->master_tune_q16, pitch_offset);
    unsigned op;

    for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
        int32_t word =
            slopsix_op_freq_from_voice(&engine->params[op], voice);

        engine->ops[op].increment =
            slopsix_increment_from_word(word, state->sample_rate);
    }
}

/*
 * The engine's note-on (the firmware's VOICE_ADD side): every operator's
 * parameters key from the live patch, the velocity composes the played
 * level into each envelope, the pitch EG resets to its initial level,
 * and the phases reset when the patch's OSC KEY SYNC bit is set - the
 * reset value is the engine's pinning of the chip-internal one
 * (research O-01).  A voice re-keyed mid-sound (the mono full trigger,
 * a stolen voice) restarts its envelopes out of silence, as the
 * firmware's key-off-then-key-on does.
 */
void slopsix_voice_engine_note_on(slopsix_context_state *state,
    slopsix_voice *slot, slopsix_voice_engine *engine)
{
    int32_t note;
    uint8_t group;

    /* The transpose is keyed with the note, so a later patch load does
     * not re-pitch a sounding voice (the patch comment's rule). */
    slot->transpose = state->patch.transpose;
    note = slopsix_effective_note(slot->note, slot->transpose);
    group = slopsix_note_group((uint8_t)note);
    unsigned k;

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        const slopsix_patch_op *pop = &state->patch.ops[k];
        slopsix_op_params *vp = &engine->params[k];
        int32_t op_level_q8;

        /* The frequency composition runs on every control-rate rewrite,
         * so the operator's oscillator parameters live in the engine. */
        vp->mode = pop->mode;
        vp->coarse = pop->coarse;
        vp->fine = pop->fine;
        vp->detune = pop->detune;
        vp->out_level = pop->out_level;
        vp->velocity_sens = pop->velocity_sens;
        vp->ams_sens = pop->ams_sens;
        vp->log_amp_q10 = 0;

        /* The level pipeline: the patch's operator level, this note's
         * keyboard scaling correction and the key's velocity compose
         * the operator's played level (research E-05, E-07).  The rate
         * scaling follows the note, not the key. */
        op_level_q8 = slopsix_op_level_q8(pop->out_level, pop->kls[group],
            slot->velocity, pop->velocity_sens);

        slopsix_eg_init(&engine->eg[k], pop->rates, pop->levels,
            op_level_q8,
            slopsix_rate_scaling((uint8_t)note, pop->rate_scaling),
            state->sample_rate);
    }

    engine->feedback_level = state->patch.feedback;

    /* The pitch EG: the patch's rate/level pairs, started at the final
     * (release) level, as the firmware's VOICE_ADD_MONO does. */
    slopsix_pitch_eg_init(&engine->pitch_eg, state->patch.peg_rates,
        state->patch.peg_levels);
    slopsix_pitch_eg_note_on(&engine->pitch_eg);

    /* The LFO's note-on side: the global delay/fade and sync resets, as
     * the firmware's voice-add paths run them on every key event. */
    slopsix_lfo_note_on(state);

    if (state->patch.osc_sync) {
        for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
            engine->ops[k].phase = 0;
        }
    }

    slopsix_voice_engine_update_frequency(state, slot, engine);

    slot->state = SLOPSIX_VOICE_ON;
    slot->held = true;
    slot->seq = ++state->voice_seq;
}

/*
 * The engine's release (the firmware's VOICE_REMOVE side): the operator
 * envelopes jump to their release stage and the pitch EG takes its
 * release step.  Runs exactly once, on the true release.
 */
void slopsix_voice_engine_release(slopsix_voice_engine *engine)
{
    unsigned k;

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        slopsix_eg_note_off(&engine->eg[k]);
    }
    slopsix_pitch_eg_note_off(&engine->pitch_eg);
}

/* The pitch state of every sounding voice: a bend, its range, the master
 * tune or the host sample rate leaves the phase increments stale, so the
 * setters rewrite them.  Voices the pedal holds and released tails are
 * sounding too. */
void slopsix_refresh_pitch(slopsix_context_state *state)
{
    unsigned i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].state == SLOPSIX_VOICE_FREE) {
            continue;
        }
        slopsix_voice_engine_update_frequency(state, &state->voices[i],
            &state->engines[i]);
    }
}

/*
 * The render call.
 *
 * Per sample: the two CPU-side cadence gates (one global OCF timer on the
 * hardware drives both - the LFO and its two register pushes on every
 * tick, the pitch EG on every second tick; the two accumulators'
 * periods are exact multiples, so the pitch EG's walk lands on every
 * second LFO tick, as the hardware's toggle does), then every sounding
 * voice's graph walk, compander word and bus contribution, and finally
 * the output stages.  A tick that reaches either gate rewrites the
 * sounding voices' phase increments, the way the hardware's portamento
 * process carries the composed pitch offset; a free voice renders
 * nothing and its operators keep the increments they were last keyed
 * with.
 */
void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    const slopsix_algorithm *alg;
    size_t sample;
    size_t i;

    if (stereo_out == NULL) {
        return;
    }
    if (state == NULL) {
        /* The render call always writes deterministic output, even on a
         * null context. */
        for (i = 0; i < frames * 2u; i++) {
            stereo_out[i] = 0;
        }
        return;
    }

    alg = slopsix_algorithm_get(state->patch.algorithm);

    for (sample = 0; sample < frames; sample++) {
        int32_t bus = 0;
        int lfo_tick;
        int peg_tick;
        unsigned v;

        lfo_tick = slopsix_lfo_cadence_tick(&state->lfo_cadence);
        peg_tick = slopsix_pitch_eg_cadence_tick(&state->pitch_cadence);

        if (lfo_tick) {
            slopsix_lfo_tick(state);
        }
        if (lfo_tick || peg_tick) {
            for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
                if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                    continue;
                }
                if (peg_tick) {
                    slopsix_pitch_eg_update(&state->engines[v].pitch_eg);
                }
                slopsix_voice_engine_update_frequency(state,
                    &state->voices[v], &state->engines[v]);
            }
        }

        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            slopsix_compander_word word;
            int32_t out;

            if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                continue;
            }

            slopsix_voice_render_sample(&state->engines[v], alg,
                state->lfo.amp_mod_register, &out);
            slopsix_compander_quantize(out, &word);
            bus += slopsix_compander_value(&word);
        }

        slopsix_output_mix(state, bus,
            &stereo_out[sample * 2u], &stereo_out[sample * 2u + 1u]);
    }
}
