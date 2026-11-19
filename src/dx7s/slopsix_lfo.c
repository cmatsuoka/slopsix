/*
 * src/dx7s/slopsix_lfo.c - the DX7S per-voice LFO engine.
 *
 * The DX7S has one LFO per voice (research dx7s §5, §18).  In the
 * personality's Multi mode every voice keeps its own phase, delay
 * accumulator, fade and sample-and-hold state, while all sixteen share
 * the patch's speed, delay, waveform, depths and sensitivity.  The
 * waveform and per-tick register recipes are the Mk1's (src/slopsix_lfo.c);
 * this file only distributes them over the voice bank.
 *
 * In Single mode the context-wide Mk1 LFO runs unchanged and these
 * functions are not called; the per-voice state stays parked.
 *
 * This translation unit runs at control rate, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* Patch activation: the same activated parameters on every voice.  The
 * runtime state is deliberately untouched, so a patch load does not
 * re-seed the per-voice phases. */
void slopsix_dx7s_lfo_patch_activate(slopsix_context_state *state,
    const uint8_t *dump)
{
    unsigned v;

    for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
        slopsix_lfo_activate(&state->engines[v].lfo, dump);
    }
}

/* One OCF tick: every voice's LFO steps.  The shared recipe does one
 * voice's delay/phase/wave/register work; the gate drives all sixteen
 * from the same sample, as the hardware's per-voice LFOs run from one
 * timer. */
void slopsix_dx7s_lfo_tick(slopsix_context_state *state)
{
    unsigned v;

    for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
        slopsix_lfo_tick_one(state, &state->engines[v].lfo);
    }
}

/* Note-on: reset only the keyed voice's LFO (a non-zero patch delay clears
 * its delay/fade pair; LFO sync seeds its phase at 0x7FFF). */
void slopsix_dx7s_lfo_note_on(slopsix_context_state *state,
    slopsix_voice_engine *engine)
{
    slopsix_lfo_note_on_one(state, &engine->lfo);
}
