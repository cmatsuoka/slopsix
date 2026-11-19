/*
 * slopsix_voice.c - The render call's driver: the control-rate cadences,
 * the voice loop and the voice engine's note-on / release / rewrite.
 *
 * The loop follows the hardware's per-sample register flow with the
 * evaluation per block: every sounding voice's operators render
 * operator-major over blocks of samples (src/slopsix_algorithm.c), each
 * voice's carrier sum goes through the compander, the quantized voices
 * sum on the bus, and the once-per-sample output stages turn the bus
 * into the two channels:
 *
 *   per sub-block (cut at the cadence ticks):
 *     for each voice:  block render -> carrier sums for the block
 *                      (int32) -> compander quantize per sample
 *                      (12-bit mantissa + shift)
 *     bus per sample (wide accumulator, plan decision D-15)
 *     output stages -> int16 L/R, per sample
 *
 * The pitch EG and the LFO are CPU-side processes on global cadences
 * (plan decision D-1): the context's gates fire at the firmware's tick
 * rates (the LFO and its register pushes on every OCF tick, the pitch EG
 * on every second tick), advance the respective staircases for the whole
 * instrument, and rewrite the voices' phase increments - the firmware's
 * PORTA_PROCESS does the same rewrite at the same ticks.  The sub-blocks
 * end exactly on the gates' samples, so the tick's updates run between
 * sub-blocks, at the same sample the per-sample loop runs them: the
 * tick's sample itself renders with the fresh register values.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
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
    /* The firmware's PORTA_PROCESS feeds the EGS a frequency word of its
     * own: the note target glides through the portamento buffer, and
     * glissando, while it is on, quantizes that to semitones.  Both
     * buffers carry the hardware's two zero low bits, so the word is
     * masked here just before the ratio and detune are added. */
    int32_t base = state->glissando ? slot->gliss_current
                                    : slot->porta_current;
    int32_t voice = slopsix_voice_word_from_base(base & ~3,
        state->master_tune_q16, pitch_offset);
    unsigned op;

    for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
        int32_t word =
            slopsix_op_freq_from_voice(&engine->params[op], voice);

        engine->ops[op].increment =
            slopsix_increment_from_word(word, state->sample_rate);
    }

    engine->pitch_offset = pitch_offset;
}

/*
 * PORTA_PROCESS over one half of the voice bank.
 *
 * The firmware does not update all sixteen voices on the same OCF tick:
 * M_PORTA_UPDATE_VOICE_SWITCH alternates the bank, so voices 0..7 move
 * on one tick and 8..15 on the next.  Every voice therefore steps at the
 * same ~187.6 Hz period, but the two banks are one tick (~2.66 ms) out
 * of phase with each other.  That is audible as a staggered staircase
 * in a polyphonic glide, so the engine reproduces it rather than
 * stepping the whole bank together: `state->porta_bank` is the same
 * toggle, flipped by the caller on every OCF tick, and only the selected
 * half is advanced here.
 *
 * The return value is a bit mask of the voices whose audible word moved;
 * the render loop rewrites just those phase increments.  A voice whose
 * glissando stair is holding still while its portamento word moves does
 * not need a rewrite, because the stair is what the EGS hears.
 */
uint16_t slopsix_porta_process(slopsix_context_state *state, unsigned bank)
{
    unsigned first = bank * (SLOPSIX_VOICE_COUNT / 2u);
    unsigned last = first + (SLOPSIX_VOICE_COUNT / 2u);
    uint16_t changed = 0;
    unsigned v;

    for (v = first; v < last; v++) {
        slopsix_voice *slot = &state->voices[v];
        int32_t before;

        if (slot->state == SLOPSIX_VOICE_FREE) {
            continue;
        }

        before = state->glissando ? slot->gliss_current
                                  : slot->porta_current;
        (void)slopsix_porta_step_word(&slot->porta_current,
            slot->porta_target, &slot->gliss_current, state->porta_rate,
            state->glissando);
        if ((state->glissando ? slot->gliss_current
                              : slot->porta_current) != before) {
            changed |= (uint16_t)(1u << v);
        }
    }

    return changed;
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

        /* The amplitude modulation's reduction, composed from the EGS's
         * amp-mod register's current value: the register changes only
         * on the LFO's ticks, and the tick handler refreshes the cached
         * value from then on. */
        vp->ams_reduction_q8 = slopsix_ams_reduction_q8(vp->ams_sens,
            state->lfo.amp_mod_register);

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
 * The engine's amplitude-modulation refresh (the EGS's amp-mod register's
 * side): the register's value changes only on the LFO's ticks, so every
 * sounding voice recomposes its operators' reductions there and the
 * render path only reads the cached values.
 */
void slopsix_voice_engine_update_ams(slopsix_context_state *state,
    slopsix_voice_engine *engine)
{
    unsigned k;

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        engine->params[k].ams_reduction_q8 =
            slopsix_ams_reduction_q8(engine->params[k].ams_sens,
                state->lfo.amp_mod_register);
    }
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

/*
 * The patch activation's silence: the firmware's VOICE_RESET_EGS writes
 * the maximum operator level to every voice and keys every EGS voice
 * off.  The engine mirrors it on the operator envelopes; the voice stays
 * in the allocation's key-event bookkeeping, so a key still held keeps
 * its slot but makes no sound until it is released and struck again.
 */
void slopsix_voice_engine_silence(slopsix_voice_engine *engine)
{
    unsigned k;

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        slopsix_eg_silence(&engine->eg[k]);
    }
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
 * The DX7 personality's render call.
 *
 * The control-rate work runs on the two global cadences (one OCF timer
 * on the hardware drives both - the LFO and its register pushes on every
 * tick, the pitch EG on every second tick), and everything else runs per
 * block: the voices render operator-major over blocks of samples and the
 * output stages turn each sample of the mixed bus into the two channels.
 * The call is cut into sub-blocks that end exactly on the cadence ticks -
 * an integer lookahead over the two cadence accumulators places the next
 * tick sample - so the sample a tick belongs to is rendered with the
 * register values that tick pushed, and inside a sub-block the increments
 * and the amplitude reductions are constants, the shape the block
 * renderer's loops want.  A tick that reaches either gate rewrites a
 * sounding voice's phase increments only when its composed pitch offset
 * has moved (the hardware's portamento process carries the composed pitch
 * offset; a static pitch needs no rewrite); a free voice renders nothing
 * and its operators keep the increments they were last keyed with.
 *
 * Per sub-block: every sounding voice's block render, each carrier sum
 * quantized by the voice's compander and summed on the bus (plan
 * decision D-15), then the once-per-sample output stages.
 *
 * This is the DX7 personality's entry point: the shared API glue calls
 * it through the context's resolved function pointer, so the sample
 * loop carries no personality branch (plan decision D-18).
 */

/* The cadence's fire threshold, the gate's own rate handling. */
static uint32_t cadence_threshold(uint32_t sample_rate)
{
    uint32_t rate = sample_rate;

    if (rate == 0u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    }
    if (rate > SLOPSIX_DEFAULT_SAMPLE_RATE * 8u) {
        rate = SLOPSIX_DEFAULT_SAMPLE_RATE * 8u;
    }

    return SLOPSIX_CADENCE_TICK_NUM * rate;
}

/* The samples until the gate fires, one-based: the accumulator fires on
 * the sample where it has crossed the threshold, so the sub-blocks can
 * end exactly on it. */
static uint32_t cadence_distance(const slopsix_cadence *cad,
                                 uint32_t threshold)
{
    uint32_t step = cad->step;

    if (step == 0u) {
        return UINT32_MAX;
    }

    return (threshold - cad->acc + step - 1u) / step;
}

/* The bulk advance over samples that do not reach a fire (the caller
 * keeps the run shorter than the distance). */
static void cadence_skip(slopsix_cadence *cad, size_t samples)
{
    cad->acc += cad->step * (uint32_t)samples;
}

/* One sample's advance; nonzero when the gate fires on it. */
static int cadence_step(slopsix_cadence *cad, uint32_t threshold)
{
    cad->acc += cad->step;

    if (cad->acc >= threshold) {
        cad->acc -= threshold;
        return 1;
    }

    return 0;
}

/* One span of the call: the voices render blocks, the per-voice
 * compander words sum on the bus, and the output stages run per sample.
 * The span is cut into the scratch's block size, and the voices are
 * processed one at a time - the scratch is one voice's worth. */
static void render_span(slopsix_context_state *state,
                        const slopsix_algorithm *alg,
                        int16_t *stereo_out, size_t start, size_t count)
{
    slopsix_block_scratch *scratch = &state->block_scratch;

    while (count > 0) {
        unsigned chunk = (count > SLOPSIX_RENDER_BLOCK)
                             ? (unsigned)SLOPSIX_RENDER_BLOCK
                             : (unsigned)count;
        int32_t *restrict bus = scratch->bus;
        int32_t *voice_out = scratch->voice_out;
        unsigned v;
        unsigned s;

        for (s = 0; s < chunk; s++) {
            bus[s] = 0;
        }

        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                continue;
            }

            slopsix_voice_render_block(&state->engines[v], alg, scratch,
                                       voice_out, chunk);

            for (s = 0; s < chunk; s++) {
                slopsix_compander_word word;

                slopsix_compander_quantize(voice_out[s], &word);
                bus[s] += slopsix_compander_value(&word);
            }
        }

        for (s = 0; s < chunk; s++) {
            slopsix_output_mix(state, bus[s],
                               &stereo_out[(start + s) * 2u],
                               &stereo_out[(start + s) * 2u + 1u]);
        }

        start += chunk;
        count -= chunk;
    }
}

void slopsix_dx7_process(slopsix_context ctx, int16_t *stereo_out,
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
        /* The render call always writes deterministic output, even on a
         * null context. */
        for (i = 0; i < frames * 2u; i++) {
            stereo_out[i] = 0;
        }
        return;
    }

    alg = slopsix_algorithm_get(state->patch.algorithm);
    lfo_threshold = cadence_threshold(state->lfo_cadence.sample_rate);
    peg_threshold = cadence_threshold(state->pitch_cadence.sample_rate);

    while (done < frames) {
        uint32_t lfo_distance = cadence_distance(&state->lfo_cadence,
                                                 lfo_threshold);
        uint32_t peg_distance = cadence_distance(&state->pitch_cadence,
                                                 peg_threshold);
        uint32_t tick = (lfo_distance < peg_distance) ? lfo_distance
                                                      : peg_distance;
        size_t left = frames - done;
        size_t plain;

        /* The samples that keep the current register values: up to the
         * one before the next tick's sample.  A tick lands at least one
         * sample ahead, so the plain run is one shorter than the
         * distance; a call that ends before the tick runs plain to its
         * end. */
        if (tick == UINT32_MAX || tick - 1u > left) {
            plain = left;
        } else {
            plain = (size_t)(tick - 1u);
        }

        if (plain > 0) {
            cadence_skip(&state->lfo_cadence, plain);
            cadence_skip(&state->pitch_cadence, plain);
            render_span(state, alg, stereo_out, done, plain);
            done += plain;
            if (done == frames) {
                break;
            }
        }

        /* The next sample is the tick's: the gates advance on it, the
         * LFO and pitch-EG handlers run on it, and the sample renders
         * with the values they pushed - the per-sample loop's order. */
        {
            int lfo_tick = cadence_step(&state->lfo_cadence, lfo_threshold);
            int peg_tick = cadence_step(&state->pitch_cadence,
                                        peg_threshold);
            uint16_t porta_changed = 0;

            if (lfo_tick) {
                slopsix_lfo_tick(state);
                /* PORTA_PROCESS runs on every OCF tick, but only over
                 * one half of the voice bank; the toggle is advanced
                 * here so the two banks keep the hardware's one-tick
                 * stagger. */
                porta_changed = slopsix_porta_process(state,
                    state->porta_bank);
                state->porta_bank ^= 1u;
            }
            if (lfo_tick || peg_tick) {
                /* The LFO's and the bend's share of the pitch offset is
                 * the same for every voice, so compose it once; the
                 * pitch EG's level is per voice.  A voice whose
                 * composed offset has not moved keeps the increments it
                 * already has, so a static pitch costs no rewrite at
                 * all.  A portamento step forces the rewrite for the
                 * voices whose audible word moved even when the bend
                 * and pitch EG did not. */
                int32_t lfo_offset = slopsix_mod_pitch_offset(state);
                unsigned v;

                for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
                    int32_t offset;

                    if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                        continue;
                    }
                    if (lfo_tick) {
                        /* The EGS's amp-mod register has a fresh
                         * value: the voice's operators' reductions
                         * follow it. */
                        slopsix_voice_engine_update_ams(state,
                            &state->engines[v]);
                    }
                    if (peg_tick) {
                        slopsix_pitch_eg_update(
                            &state->engines[v].pitch_eg);
                    }
                    offset = lfo_offset +
                        slopsix_pitch_eg_offset(&state->engines[v].pitch_eg);
                    if (((porta_changed >> v) & 1u) != 0u ||
                        offset != state->engines[v].pitch_offset) {
                        slopsix_voice_engine_update_frequency(state,
                            &state->voices[v], &state->engines[v]);
                    }
                }
            }
        }

        render_span(state, alg, stereo_out, done, 1);
        done += 1;
    }
}
