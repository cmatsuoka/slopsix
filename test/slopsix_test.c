/*
 * slopsix_test - the desktop test harness.
 *
 * Each stage is a self-contained check; the harness runs them all by
 * default, or one by name.  Stages print their results on stdout and the
 * harness exits non-zero if any check fails.
 *
 * Stage names:
 *   init     context creation, reset and destruction
 *   silence  the render path writes silence into a sentinel-filled buffer
 *   tables   the generated tables match their recipes (canary entries)
 *   voices      voice state transitions, sustain pedal logic and stealing
 *   monovoice   mono mode: last-key priority, legato, the key-event list
 *   operator    the operator core against the closed sine recipe
 *   freq        the frequency path: words, grid and pitch
 *   algorithms  the 32 algorithms' decoded connections
 *   graph       the operator-graph walk
 *   bend        the pitch bend: the curve, its quantisation and its scale
 *   pitcheg     the pitch envelope: its tables, stage walk and cadence
 *   porta       portamento and glissando: the rate/step law, the half-bank
 *               cadence and the poly/mono mode matrix
 *   lfo         the LFO: laws, waveforms, the two registers and cadence
 *   plfo        the DX7S per-voice LFO: shared recipes, per-voice delay
 *              and reset, the stagger and neutral Single-mode parity
 *   pegr        the DX7S's pitch-EG composition: the range, the velocity
 *              switch and the rate scaling, and the DX7's gate
 *   unison      the DX7S key modes: fixed groups, the steal, the pedal,
 *              detune and the stack's superposition
 *   params      the DX7S-only parameters: gated per personality, their
 *              ranges and the neutral defaults
 *   ctrl        the controllers: scaling, routes and both registers
 *   filter      the analog tail: the reconstruction filter's design,
 *               its DC gain and its response
 *   output      the output stages: compander, DAC step, pan law, master
 *               volume and the host output volume
 *   api         the public API: engine selection, the patch I/O, the
 *               setters' error reporting and the capability queries
 *   extras      the pan is an extra: available or a no-op by build
 *   render      the live voice path: patch parse, note render and release
 *   blockgraph  the block renderer against the walk, per algorithm and
 *               span shape (voice level)
 *   blockprocess the block render call against the reference per-sample
 *               loop, through the public API, over the splits and the
 *               cadence ticks' positions
 *
 * The harness is a host tool, not the library: it may use whatever it
 * likes, but it links the public library plus the internal declarations
 * from the internal header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slopsix_internal.h"

static int failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static void stage_init(void)
{
    slopsix_context ctx;
    int16_t buffer[8];

    printf("stage init\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, 0);
    check(ctx != NULL, "create_context(0) returns a context");
    if (ctx == NULL) {
        return;
    }

    slopsix_reset(ctx);
    check(true, "reset runs");

    slopsix_set_sample_rate(ctx, 48000);
    check(true, "set_sample_rate runs");

    memset(buffer, 0, sizeof(buffer));
    slopsix_process(ctx, buffer, 4);
    check(true, "process runs on an empty context");

    slopsix_destroy_context(ctx);
    check(true, "destroy_context runs");
}

static void stage_silence(void)
{
    slopsix_context ctx;
    int16_t buffer[64 * 2];
    size_t i;
    int nonzero = 0;

    printf("stage silence\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }

    for (i = 0; i < 64 * 2u; i++) {
        buffer[i] = (int16_t)0x5A5A;
    }
    slopsix_process(ctx, buffer, 64);

    for (i = 0; i < 64 * 2u; i++) {
        if (buffer[i] != 0) {
            nonzero++;
        }
    }
    check(nonzero == 0, "the render path writes silence into a sentinel buffer");

    slopsix_destroy_context(ctx);
}

static void stage_tables(void)
{
    int i;
    int monotonic_logsin = 1;
    int monotonic_exp = 1;
    int octave_exact = 1;

    printf("stage tables\n");

    /* Log-sine quarter wave: the canary entry n=233 carries the
     * hardware's +0.5002 rounding bias; the table runs from 10597 (the
     * steep asymptote) down to 0. */
    check(slopsix_logsin_quarter[0] == 10597, "log-sine[0] == 10597");
    check(slopsix_logsin_quarter[1023] == 0, "log-sine[1023] == 0");
    check(slopsix_logsin_quarter[233] == 1549, "log-sine[233] == 1549 (canary)");
    for (i = 0; i < 1023; i++) {
        if (slopsix_logsin_quarter[i] < slopsix_logsin_quarter[i + 1]) {
            monotonic_logsin = 0;
        }
    }
    check(monotonic_logsin, "log-sine is monotonically nonincreasing");

    /* Exponential ROM: 2048 (frac 0) to 4093 (frac 1023), Q11. */
    check(slopsix_exp_q11[0] == 2048, "exp[0] == 2048");
    check(slopsix_exp_q11[1023] == 4093, "exp[1023] == 4093");
    for (i = 0; i < 1023; i++) {
        if (slopsix_exp_q11[i] > slopsix_exp_q11[i + 1]) {
            monotonic_exp = 0;
        }
    }
    check(monotonic_exp, "exp is monotonically increasing");

    /* Note table: the firmware's key-code bytes and the composed word. */
    check(slopsix_note_code[0] == 0, "note_code[0] == 0 (key-code-0 default)");
    check(slopsix_note_code[1] == 0, "note_code[1] == 0");
    check(slopsix_note_code[13] == 16, "note_code[13] == 16");
    check(slopsix_note_code[127] == 168, "note_code[127] == 168");
    check(slopsix_note_word[13] == 4096,
          "note_word[13] == 4096 (one octave, 4096 units per octave)");
    check(slopsix_note_word[69] == 23208,
          "note_word[69] == 23208 (the ROM's word for A4)");
    /* The octave property holds from note 1 up: key code 0 is the safe
     * default and does not continue the series. */
    for (i = 1; i + 12 < 128; i++) {
        if ((int)slopsix_note_word[i + 12] - (int)slopsix_note_word[i] != 4096) {
            octave_exact = 0;
        }
    }
    check(octave_exact, "every octave spans exactly 4096 word units");

    /* COM: the hardware's six 6-bit log patterns. */
    check(slopsix_com_log[0] == 0, "com[0] == 0");
    check(slopsix_com_log[1] == 16, "com[1] == 16");
    check(slopsix_com_log[2] == 26, "com[2] == 26");
    check(slopsix_com_log[3] == 32, "com[3] == 32");
    check(slopsix_com_log[4] == 38, "com[4] == 38");
    check(slopsix_com_log[5] == 42, "com[5] == 42");
}

static int count_voices(slopsix_context_state *state, uint8_t note,
                        slopsix_voice_state which)
{
    size_t i;
    int n = 0;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].note == note &&
            state->voices[i].state == which) {
            n++;
        }
    }
    return n;
}

static void stage_voices(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;

    printf("stage voices\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    /* Note on, pedal down, note off. */
    slopsix_note_on(ctx, 60, 100);
    check(count_voices(state, 60, SLOPSIX_VOICE_ON) == 1,
          "note_on sets voice to SLOPSIX_VOICE_ON");

    slopsix_set_sustain(ctx, true);
    slopsix_note_off(ctx, 60);
    check(count_voices(state, 60, SLOPSIX_VOICE_ON) == 1,
          "note_off with sustain keeps voice in SLOPSIX_VOICE_ON");

    /* Retrigger the same note while the pedal holds the first voice.
     * Releasing the pedal must release the first voice but keep the
     * newly held one. */
    slopsix_note_on(ctx, 60, 100);
    check(count_voices(state, 60, SLOPSIX_VOICE_ON) == 2,
          "retrigger under sustain allocates a second ON voice");

    slopsix_set_sustain(ctx, false);
    check(count_voices(state, 60, SLOPSIX_VOICE_ON) == 1,
          "releasing sustain keeps only the still-held voice ON");
    check(count_voices(state, 60, SLOPSIX_VOICE_RELEASED) == 1,
          "releasing sustain releases the pedal-held voice");

    /* A plain note off releases the remaining voice. */
    slopsix_note_off(ctx, 60);
    check(count_voices(state, 60, SLOPSIX_VOICE_ON) == 0,
          "note_off without sustain releases the held voice");

    slopsix_destroy_context(ctx);

    /* A second note-off of a pedal-held note defers silently: the
     * firmware's voice entries lose their active bit at the first
     * release, so the second one finds nothing to release. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    slopsix_note_on(ctx, 72, 100);
    slopsix_set_sustain(ctx, true);
    slopsix_note_off(ctx, 72);
    check(count_voices(state, 72, SLOPSIX_VOICE_ON) == 1,
          "the pedal holds the released voice");
    slopsix_note_off(ctx, 72);
    check(count_voices(state, 72, SLOPSIX_VOICE_ON) == 1,
          "a second note-off under the pedal releases nothing");
    slopsix_set_sustain(ctx, false);
    check(count_voices(state, 72, SLOPSIX_VOICE_RELEASED) == 1,
          "pedal-up releases the held voice");

    /* All-notes-off acts as if every key were released: the pedal keeps
     * the voice, without the pedal it releases. */
    slopsix_note_on(ctx, 48, 100);
    slopsix_note_on(ctx, 50, 100);
    slopsix_set_sustain(ctx, true);
    slopsix_note_off(ctx, 50);
    slopsix_all_notes_off(ctx);
    check(count_voices(state, 48, SLOPSIX_VOICE_ON) == 1 &&
          count_voices(state, 50, SLOPSIX_VOICE_ON) == 1,
          "all-notes-off under the pedal keeps both voices");
    check(count_voices(state, 48, SLOPSIX_VOICE_RELEASED) == 0,
          "all-notes-off does not release pedal-held voices");
    slopsix_set_sustain(ctx, false);
    check(count_voices(state, 48, SLOPSIX_VOICE_RELEASED) == 1,
          "pedal-up after all-notes-off releases the voice");

    /* Voice stealing: oldest-first, both over key-holding voices and
     * over release tails.  A fresh context keeps the count exact. */
    {
        slopsix_context steal_ctx;
        slopsix_context_state *steal_state;
        unsigned n;

        steal_ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
        if (steal_ctx == NULL) {
            check(0, "create_context returns a context");
            return;
        }
        steal_state = SLOPSIX_CONTEXT(steal_ctx);

        for (n = 0; n < SLOPSIX_VOICE_COUNT; n++) {
            slopsix_note_on(steal_ctx, (uint8_t)(40u + n), 100);
        }
        check(count_voices(steal_state, 40, SLOPSIX_VOICE_ON) == 1 &&
              count_voices(steal_state, 55, SLOPSIX_VOICE_ON) == 1,
              "sixteen notes fill the sixteen voices");
        slopsix_note_on(steal_ctx, 70, 100);
        check(steal_state->voices[0].note == 70 &&
              steal_state->voices[0].state == SLOPSIX_VOICE_ON,
              "the seventeenth note steals the oldest voice");
        check(count_voices(steal_state, 41, SLOPSIX_VOICE_ON) == 1,
              "the stolen voice released nothing else");

        for (n = 0; n < SLOPSIX_VOICE_COUNT; n++) {
            slopsix_note_off(steal_ctx, (uint8_t)(40u + n));
        }
        slopsix_note_off(steal_ctx, 70);
        check(count_voices(steal_state, 41, SLOPSIX_VOICE_RELEASED) == 1,
              "the release tails ring");

        slopsix_note_on(steal_ctx, 72, 100);
        check(steal_state->voices[1].note == 72 &&
              steal_state->voices[1].state == SLOPSIX_VOICE_ON,
              "a new note takes an old release tail");

        slopsix_destroy_context(steal_ctx);
    }

    slopsix_destroy_context(ctx);
}

/* --------------------------------------------------------- mono mode
 *
 * The firmware's monophonic bookkeeping: a list of up to sixteen key
 * events, one sounding voice (voice 0), a legato direction, and the
 * last key the next gesture compares against.  The stage walks the
 * gestures of the firmware's VOICE_ADD_MONO / VOICE_REMOVE_MONO and
 * checks the sounding note and the untouched envelopes between them. */
static void stage_monovoice(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    int32_t level_before;
    uint8_t stage_before;
    int i;

    printf("stage monovoice\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_mono_mode(ctx, true);

    /* The first key after silence is a full trigger on voice 0. */
    slopsix_note_on(ctx, 60, 100);
    check(state->voices[0].state == SLOPSIX_VOICE_ON &&
          state->voices[0].held &&
          state->voices[0].note == 60 &&
          state->voices[0].velocity == 100,
          "the first mono key full-triggers voice 0");
    check(state->engines[0].eg[0].level == 0,
          "the full trigger starts the envelope out of silence");
    check(state->engines[0].pitch_eg.step == 0u,
          "the full trigger resets the pitch EG");

    /* Note 0 is a valid key: it must not match a free list slot. */
    slopsix_note_off(ctx, 0);
    check(state->mono_count == 1 &&
          state->voices[0].note == 60 &&
          state->voices[0].state == SLOPSIX_VOICE_ON,
          "key-off 0 leaves a held key and the free slots alone");

    /* The transpose is keyed with the note; a later patch change does
     * not re-pitch the sounding voice. */
    {
        uint32_t inc = state->engines[0].ops[0].increment;

        state->patch.transpose = 12;
        slopsix_refresh_pitch(state);
        check(state->engines[0].ops[0].increment == inc,
              "a sounding voice keeps its keyed transpose");
        state->patch.transpose = 0;
        slopsix_refresh_pitch(state);
    }

    /* Walk the envelope by hand so it sits mid-flight; the legato check
     * then means something (a retrigger would reset the level). */
    for (i = 0; i < 20; i++) {
        slopsix_eg_step(&state->engines[0].eg[0]);
    }
    level_before = state->engines[0].eg[0].level;
    stage_before = state->engines[0].eg[0].stage;

    /* The second key sets the legato direction and re-pitches. */
    slopsix_note_on(ctx, 64, 100);
    check(state->voices[0].note == 64,
          "the second key re-pitches the sounding voice");
    check(state->engines[0].eg[0].level == level_before &&
          state->engines[0].eg[0].stage == stage_before,
          "the legato re-pitch does not retrigger the envelope");
    check(state->voices[0].velocity == 100,
          "the legato re-pitch keeps the first key's velocity");

    /* The direction extends upward: every higher key sounds. */
    slopsix_note_on(ctx, 65, 100);
    slopsix_note_on(ctx, 67, 100);
    check(state->voices[0].note == 67, "an extending key sounds");
    check(state->mono_count == 4, "every key event joins the list");

    /* A non-extending key does not move the sounding pitch. */
    slopsix_note_on(ctx, 63, 50);
    check(state->voices[0].note == 67 &&
          state->voices[0].velocity == 100,
          "a non-extending key does not sound");

    /* Key-off falls back along the legato direction: the highest of
     * the remaining keys, here. */
    slopsix_note_off(ctx, 67);
    check(state->voices[0].note == 65, "key-off falls back to the highest");
    slopsix_note_off(ctx, 65);
    check(state->voices[0].note == 64, "the fallback walks down");
    slopsix_note_off(ctx, 64);
    check(state->voices[0].note == 63, "the fallback walks down");
    slopsix_note_off(ctx, 63);
    check(state->voices[0].note == 60, "the fallback walks down");
    check(state->engines[0].eg[0].stage == stage_before,
          "the fallbacks never retrigger the envelope");

    /* The last key off sounds the release. */
    slopsix_note_off(ctx, 60);
    check(state->voices[0].state == SLOPSIX_VOICE_RELEASED &&
          state->mono_count == 0,
          "the last key off releases the voice");

    /* Key-off 0 with an empty list is ignored, not a free-slot hit. */
    slopsix_note_off(ctx, 0);
    check(state->mono_count == 0 &&
          state->voices[0].state == SLOPSIX_VOICE_RELEASED,
          "key-off 0 with an empty list is ignored");

    /* A release tail: the next first key re-triggers over it. */
    slopsix_note_on(ctx, 60, 100);
    check(state->engines[0].eg[0].level == 0,
          "a retrigger after silence starts out of silence");

    /* The sustain pedal defers only the final release; the intermediate
     * keys leave the list all the same, and the fallback keeps walking. */
    slopsix_note_on(ctx, 64, 100);
    slopsix_set_sustain(ctx, true);
    slopsix_note_off(ctx, 64);
    check(state->voices[0].note == 60,
          "the pedal-down key-off falls back to the held key");
    slopsix_note_off(ctx, 60);
    check(state->voices[0].state == SLOPSIX_VOICE_ON &&
          !state->voices[0].held,
          "the pedal defers the final release");
    slopsix_set_sustain(ctx, false);
    check(state->voices[0].state == SLOPSIX_VOICE_RELEASED,
          "pedal-up releases the deferred voice");

    /* The key-event list caps at sixteen events.  An ascending run
     * sounds the highest key so far. */
    slopsix_note_on(ctx, 40, 100);
    for (i = 0; i < 15; i++) {
        slopsix_note_on(ctx, (uint8_t)(41u + i), 100);
    }
    check(state->mono_count == 16 && state->voices[0].note == 55,
          "sixteen key events fill the list, the run sounds its top");
    slopsix_note_on(ctx, 80, 100);
    check(state->mono_count == 16 && state->voices[0].note == 55,
          "a key past the sixteenth event is ignored");
    slopsix_set_sustain(ctx, true);
    for (i = 15; i >= 0; i--) {
        slopsix_note_off(ctx, (uint8_t)(40u + i));
    }
    check(state->voices[0].note == 40 && !state->voices[0].held,
          "the pedal holds the last release");
    slopsix_set_sustain(ctx, false);
    check(state->voices[0].state == SLOPSIX_VOICE_RELEASED,
          "pedal-up releases it");

    /* All-notes-off in mono: the list clears, the voice releases
     * (or defers under the pedal). */
    slopsix_note_on(ctx, 55, 100);
    slopsix_note_on(ctx, 57, 100);
    slopsix_all_notes_off(ctx);
    check(state->mono_count == 0 &&
          state->voices[0].state == SLOPSIX_VOICE_RELEASED,
          "all-notes-off clears the mono list and releases");

    slopsix_destroy_context(ctx);
}


/* ------------------------------------------------- Phase 2: the operator */

/* The reference operator: the closed sine recipe, written out again in
 * the harness so that the library's operator is checked against the
 * recipe and not against itself. */
static int32_t reference_sample(uint32_t phase, int32_t log_amp_q10)
{
    uint32_t sine_phase = (phase >> 11) & 0xFFFu;
    uint32_t quarter = sine_phase & 0x3FFu;
    int32_t log_word;
    int32_t sample;

    if (sine_phase & 0x400u) {
        quarter = 1023u - quarter;
    }

    log_word = log_amp_q10 - (int32_t)slopsix_logsin_quarter[quarter];

    if (log_word < 0) {
        sample = 0;
    } else {
        int32_t int_part = log_word >> 10;
        int32_t frac = log_word & 1023;

        if (int_part > 15) {
            int_part = 15;
            frac = 1023;
        }
        sample = (int32_t)(((uint32_t)slopsix_exp_q11[frac] <<
                            (uint32_t)int_part) >> 13);
    }

    if (sine_phase & 0x800u) {
        sample = -sample;
    }

    return sample;
}

static void stage_operator(void)
{
    slopsix_operator op;
    slopsix_op_params params;
    int note;
    int mismatch = 0;
    int32_t peak = 0;

    printf("stage operator\n");

    /* A pure carrier: the operator's output must follow the closed sine
     * recipe exactly, sample for sample. */
    memset(&params, 0, sizeof(params));
    params.mode = 0;
    params.coarse = 1;
    params.fine = 0;
    params.detune = 7;
    params.log_amp_q10 = 4095 << SLOPSIX_AMP_Q8_SHIFT;

    for (note = 21; note <= 108; note += 3) {
        int32_t word;
        uint32_t increment;
        uint32_t phase = 0;
        int i;

        memset(&op, 0, sizeof(op));

        word = slopsix_op_freq_word((uint8_t)note, &params, 0, 0);
        increment = slopsix_increment_from_word(word,
                                                SLOPSIX_DEFAULT_SAMPLE_RATE);
        op.increment = increment;

        for (i = 0; i < 4096; i++) {
            int32_t got;
            int32_t want;

            slopsix_op_render(&op, params.log_amp_q10, 0, &got);
            want = reference_sample(phase, params.log_amp_q10);

            if (got != want) {
                mismatch++;
            }
            if (got > peak) {
                peak = got;
            }

            phase = (phase + increment) & SLOPSIX_PHASE_MASK;
        }
    }

    check(mismatch == 0,
          "the operator matches the closed sine recipe sample for sample");
    check(peak > 16000,
          "a full-scale carrier reaches the 14-bit output's top (>= 16000)");

    /* The modulation scale: a modulator's linear output is added to the
     * sine phase index, where one period is 4096 units.  A 4096-unit
     * modulation therefore repeats the wave, and 2048 units inverts it. */
    {
        slopsix_operator modop;
        int32_t base;
        int32_t one_period;
        int32_t half_period;

        memset(&modop, 0, sizeof(modop));
        modop.increment = 0;
        slopsix_op_render(&modop, params.log_amp_q10, 0, &base);

        memset(&modop, 0, sizeof(modop));
        modop.increment = 0;
        slopsix_op_render(&modop, params.log_amp_q10, 4096, &one_period);

        memset(&modop, 0, sizeof(modop));
        modop.increment = 0;
        slopsix_op_render(&modop, params.log_amp_q10, 2048, &half_period);

        check(one_period == base,
              "a 4096-unit modulation is one sine period");
        check(half_period == -base,
              "a 2048-unit modulation is half a sine period");
    }
}

/* The frequency of a phase increment, in milli-hertz, computed with
 * integer arithmetic only. */
static int64_t increment_millihertz(uint32_t increment, uint32_t rate)
{
    /* f = increment * rate / 2^23 Hz, in milli-hertz. */
    return ((int64_t)increment * (int64_t)rate * 1000) /
           ((int64_t)1 << SLOPSIX_PHASE_BITS);
}

/* The expected frequency of a note on the T-law curve, in milli-hertz:
 * 440 Hz scaled by the note word's offset from A4. */
static int64_t expected_millihertz(int32_t word)
{
    /* The offset from A4 in 1024ths of an octave. */
    int32_t octave_1024 = (word - SLOPSIX_WORD_A4) / 4;
    int32_t int_part = octave_1024 >> 10;
    int32_t frac = octave_1024 & 1023;
    uint32_t frac_value = slopsix_exp_q11[frac];
    int64_t ratio_q16;  /* 2^(frac/1024) in Q16 */

    ratio_q16 = ((int64_t)frac_value * 65536) / 2048;

    if (int_part >= 0) {
        ratio_q16 <<= int_part;
    } else {
        ratio_q16 >>= (-int_part);
    }

    /* ratio_q16 is 2^(the offset in octaves) in Q16. */
    return (440LL * 1000LL * ratio_q16) / 65536LL;
}

static void stage_freq(void)
{
    slopsix_op_params params;
    int note;
    int a4_ok = 1;
    int cents_ok = 1;
    int grid_ok = 1;
    int octave_ok = 1;
    int64_t a4_freq = 0;
    int64_t frequencies[128];

    printf("stage freq\n");

    memset(&params, 0, sizeof(params));
    params.mode = 0;
    params.coarse = 1;
    params.fine = 0;
    params.detune = 7;

    for (note = 0; note < 128; note++) {
        int32_t word = slopsix_op_freq_word((uint8_t)note, &params, 0, 0);
        uint32_t increment = slopsix_increment_from_word(
            word, SLOPSIX_DEFAULT_SAMPLE_RATE);
        int64_t freq = increment_millihertz(increment,
                                            SLOPSIX_DEFAULT_SAMPLE_RATE);
        int64_t want = expected_millihertz(word);
        int64_t diff = freq - want;

        if (diff < 0) {
            diff = -diff;
        }

        /* The engine's pitch is quantized twice: to the EGS's
         * 1024-per-octave field (up to 1.17 cents) and by the frequency
         * exponent's own step (up to 1.5 cents at the top of the range),
         * so the rendered frequency can sit a few cents away from the
         * T-law's value. */
        if (want > 0 && diff * 1731 > want * 3) {
            cents_ok = 0;
        }

        /* The increment must be one the hardware's frequency exponent can
         * produce: (round(2^frac * 2048) << int) >> 5. */
        {
            int32_t datum = ((word - SLOPSIX_FREQ_BIAS) >> 2) + 6411;
            uint32_t expect;

            if (datum < 0) {
                datum = 0;
            }
            if (datum > 16383) {
                datum = 16383;
            }
            expect = ((uint32_t)slopsix_exp_q11[datum & 1023] <<
                      (uint32_t)(datum >> 10)) >> 5;
            if (expect != increment) {
                grid_ok = 0;
            }
        }

        if (note == 69) {
            a4_freq = freq;
            /* The closest frequency the hardware's exponent can produce
             * for A4 is 440.12 Hz: the field's step is 32 phase units,
             * about 0.19 Hz here. */
            if (freq < 439900LL || freq > 440200LL) {
                a4_ok = 0;
            }
        }
        if (note >= 13) {
            /* Twelve notes up is one octave: the frequency doubles.  The
             * series starts at note 1: key code 0 is a safe default that
             * duplicates note 1 and does not continue the series. */
            int64_t diff = freq - 2 * frequencies[note - 12];

            if (diff < 0) {
                diff = -diff;
            }
            if (diff * 1000 > freq) {
                octave_ok = 0;
            }
        }
        frequencies[note] = freq;
    }

    check(a4_ok, "MIDI note 69 renders at 440 Hz (within the field's step)");
    check(cents_ok,
          "every note's frequency is within 3 cents of the T-law curve");
    check(grid_ok,
          "every increment is one the hardware's frequency exponent produces");
    check(octave_ok, "twelve notes up doubles the frequency");
    check(a4_freq > 0, "the frequency path returns values");

    /* The master tune moves the pitch by the requested number of cents. */
    {
        int32_t word_flat = slopsix_op_freq_word(69, &params, 0, 0);
        int32_t word_sharp = slopsix_op_freq_word(69, &params, 100 << 16, 0);
        uint32_t inc_flat = slopsix_increment_from_word(
            word_flat, SLOPSIX_DEFAULT_SAMPLE_RATE);
        uint32_t inc_sharp = slopsix_increment_from_word(
            word_sharp, SLOPSIX_DEFAULT_SAMPLE_RATE);
        int64_t f_flat = increment_millihertz(inc_flat,
                                              SLOPSIX_DEFAULT_SAMPLE_RATE);
        int64_t f_sharp = increment_millihertz(inc_sharp,
                                               SLOPSIX_DEFAULT_SAMPLE_RATE);
        int64_t cents = (f_sharp - f_flat) * 1731 / f_flat;

        check(cents >= 98 && cents <= 102,
              "a master tune of +100 cents raises the pitch by 100 cents");
    }

    /* The pitch is rate-independent: the anchor-rate increment, rescaled
     * for the host rate, must render the same frequency at every rate.
     * This is reciprocal (anchor / rate); scaling by rate / anchor
     * instead makes the pitch follow the rate squared. */
    {
        static const uint32_t rates[] = { 44100u, 48000u, 88200u, 96000u };
        slopsix_op_params base = params;
        int32_t word = slopsix_op_freq_word(69, &base, 0, 0);
        int64_t f_anchor = increment_millihertz(
            slopsix_increment_from_word(word, SLOPSIX_DEFAULT_SAMPLE_RATE),
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        int rate_ok = 1;
        unsigned i;

        for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            uint32_t inc = slopsix_increment_from_word(word, rates[i]);
            int64_t f = increment_millihertz(inc, rates[i]);
            int64_t diff = f - f_anchor;

            if (diff < 0) {
                diff = -diff;
            }
            if (diff * 1731 > f_anchor) {
                rate_ok = 0;
            }
        }
        check(rate_ok, "the rendered pitch is the same at every sample rate");

        /* A host rate below about 48 Hz wraps the rescaled increment of
         * the top pitches, so the degenerate rates are checked on the
         * top key's word: at one hertz the increment saturates instead
         * of wrapping, and a zero rate has no rescale - it is the
         * anchor, as the cadence gate treats it. */
        {
            int32_t word_high = slopsix_op_freq_word(127, &base, 0, 0);

            check(slopsix_increment_from_word(word_high, 1u) == UINT32_MAX,
                  "a one-hertz rate saturates the increment");
        }
        check(slopsix_increment_from_word(word, 0u) ==
                  slopsix_increment_from_word(word,
                                              SLOPSIX_DEFAULT_SAMPLE_RATE),
              "a zero rate rescales like the anchor rate");
    }

    /* The firmware's fine table has 101 words: a serialised fine value
     * of 100 reads the extra 0x1000 word.  Coarse 0/fine 100 is
     * therefore the 1.0 ratio, not coarse 0/fine 0's 0.5 ratio.  This
     * matters for ROM 3B program 14, whose carrier uses that encoding. */
    {
        slopsix_op_params low = params;
        slopsix_op_params high = params;

        low.coarse = 0;
        low.fine = 100;
        high.coarse = 1;
        high.fine = 0;

        check(slopsix_op_freq_word(69, &low, 0, 0) ==
              slopsix_op_freq_word(69, &high, 0, 0),
              "ratio coarse 0/fine 100 is the 1.0 ratio");
    }

    /* The fixed-frequency mode ignores the note. */
    {
        slopsix_op_params fixed = params;
        int64_t f_low;
        int64_t f_high;

        fixed.mode = 1;
        fixed.coarse = 2;
        fixed.fine = 0;

        f_low = increment_millihertz(
            slopsix_increment_from_word(
                slopsix_op_freq_word(30, &fixed, 0, 0),
                SLOPSIX_DEFAULT_SAMPLE_RATE),
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        f_high = increment_millihertz(
            slopsix_increment_from_word(
                slopsix_op_freq_word(90, &fixed, 0, 0),
                SLOPSIX_DEFAULT_SAMPLE_RATE),
            SLOPSIX_DEFAULT_SAMPLE_RATE);

        check(f_low == f_high,
              "a fixed-frequency operator ignores the note");
        check(f_low > 99000LL && f_low < 101000LL,
              "the fixed frequency of coarse 2 is 100 Hz");

        /* The coarse field is a decade selector: one step multiplies
         * the frequency by ten. */
        {
            int64_t f_coarse[4];
            unsigned coarse;
            int decade_ok = 1;

            for (coarse = 0; coarse < 4; coarse++) {
                fixed.coarse = (uint8_t)coarse;
                fixed.fine = 0;
                f_coarse[coarse] = increment_millihertz(
                    slopsix_increment_from_word(
                        slopsix_op_freq_word(69, &fixed, 0, 0),
                        SLOPSIX_DEFAULT_SAMPLE_RATE),
                    SLOPSIX_DEFAULT_SAMPLE_RATE);
            }
            for (coarse = 0; coarse < 3; coarse++) {
                int64_t ratio = f_coarse[coarse + 1] * 1000 /
                                f_coarse[coarse];

                if (ratio < 9800 || ratio > 10200) {
                    decade_ok = 0;
                }
            }
            check(decade_ok,
                  "each fixed-coarse step multiplies the frequency by ten");
        }

        /* The fixed fine term is the raw serialised value (0..127), not
         * the ratio table's index: fine 100 adds one decade. */
        {
            slopsix_op_params fixed_fine = fixed;
            int64_t f_fine100;
            int64_t f_coarse2;

            fixed_fine.mode = 1;
            fixed_fine.coarse = 1;
            fixed_fine.fine = 100;
            f_fine100 = increment_millihertz(
                slopsix_increment_from_word(
                    slopsix_op_freq_word(69, &fixed_fine, 0, 0),
                    SLOPSIX_DEFAULT_SAMPLE_RATE),
                SLOPSIX_DEFAULT_SAMPLE_RATE);

            fixed_fine.coarse = 2;
            fixed_fine.fine = 0;
            f_coarse2 = increment_millihertz(
                slopsix_increment_from_word(
                    slopsix_op_freq_word(69, &fixed_fine, 0, 0),
                    SLOPSIX_DEFAULT_SAMPLE_RATE),
                SLOPSIX_DEFAULT_SAMPLE_RATE);

            check(f_fine100 == f_coarse2,
                  "fixed fine 100 adds one decade without wrapping");
        }

        /* The fixed mode shares the ratio mode's antisymmetric detune
         * law: flat detune lowers the pitch, with the same magnitude. */
        {
            slopsix_op_params fixed_detuned = params;
            int32_t w_neutral;
            int32_t w_flat;
            int32_t w_sharp;

            fixed_detuned.mode = 1;
            fixed_detuned.coarse = 2;
            fixed_detuned.fine = 0;

            fixed_detuned.detune = 7;
            w_neutral = slopsix_op_freq_word(69, &fixed_detuned, 0, 0);
            fixed_detuned.detune = 0;
            w_flat = slopsix_op_freq_word(69, &fixed_detuned, 0, 0);
            fixed_detuned.detune = 14;
            w_sharp = slopsix_op_freq_word(69, &fixed_detuned, 0, 0);

            check(w_flat < w_neutral && w_sharp > w_neutral,
                  "the fixed mode's flat detune lowers the pitch");
            {
                int32_t flat_amount = w_neutral - w_flat;
                int32_t sharp_amount = w_sharp - w_neutral;

                check(flat_amount <= sharp_amount + 2 &&
                      sharp_amount <= flat_amount + 2,
                      "the fixed mode's detune magnitudes are symmetric");
            }
        }

        /* The fixed mode uses its own frequency register: the voice
         * pitch (and therefore the master tune) does not move it. */
        {
            slopsix_op_params fixed_tune = params;

            fixed_tune.mode = 1;
            fixed_tune.coarse = 2;
            fixed_tune.fine = 0;
            fixed_tune.detune = 7;
            check(slopsix_op_freq_word(69, &fixed_tune, 0, 0) ==
                  slopsix_op_freq_word(69, &fixed_tune, 65536, 0),
                  "the fixed mode ignores the master tune");
        }
    }

    /* The detune is zero at the hardware's neutral value and antisymmetric
     * around it. */
    {
        slopsix_op_params detuned = params;

        detuned.detune = 7;
        check(slopsix_op_freq_word(69, &detuned, 0, 0) ==
              slopsix_op_freq_word(69, &params, 0, 0),
              "the neutral detune adds nothing");

        detuned.detune = 14;
        {
            int32_t high = slopsix_op_freq_word(69, &detuned, 0, 0);
            int32_t low;

            detuned.detune = 0;
            low = slopsix_op_freq_word(69, &detuned, 0, 0);
            check(high > low, "a higher detune raises the pitch");
            check((high - low) < 64,
                  "the detune offset stays within a cent of the word");
        }
    }
}


/* --------------------------------------------------- Phase 2: the graph */

/* Build a voice's engine for one algorithm: every operator at the given
 * level, the note's frequency, and no modulation level differences. */
static void build_voice(slopsix_voice_engine *voice, unsigned algorithm,
                        uint8_t note, int32_t level_q8)
{
    unsigned op;

    memset(voice, 0, sizeof(*voice));

    for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
        voice->params[op].mode = 0;
        voice->params[op].coarse = 1;
        voice->params[op].fine = 0;
        voice->params[op].detune = 7;
        voice->params[op].log_amp_q10 = level_q8 << SLOPSIX_AMP_Q8_SHIFT;
        voice->ops[op].increment = slopsix_increment_from_word(
            slopsix_op_freq_word(note, &voice->params[op], 0, 0),
            SLOPSIX_DEFAULT_SAMPLE_RATE);
    }

    /* The feedback level belongs to the operator that carries the
     * feedback path; the level itself is the voice's own field. */
    voice->feedback_level = 0;
    (void)algorithm;
}

static void stage_graph(void)
{
    slopsix_voice_engine voice;
    slopsix_voice_engine plain;
    int32_t samples[1024];
    int nonzero = 0;
    int differs = 0;
    int deterministic = 1;
    int i;

    printf("stage graph\n");

    /* Algorithm 1: the chain 6 -> 5 -> 4 -> 3 with the feedback on
     * operator 6, and 2 -> 1.  Operator 3 is a carrier. */
    build_voice(&voice, 0, 60, 4095);
    build_voice(&plain, 0, 60, 4095);

    /* The plain voice has no modulator: operator 4's level is zero, so
     * operator 3 is an unmodulated carrier. */
    plain.params[3].log_amp_q10 = 0;

    for (i = 0; i < 1024; i++) {
        int32_t out;
        int32_t plain_out;

        slopsix_voice_render_sample(&voice,
                                    slopsix_algorithm_get(0), &out);
        slopsix_voice_render_sample(&plain,
                                    slopsix_algorithm_get(0),
                                    &plain_out);
        samples[i] = out;

        if (out != 0) {
            nonzero++;
        }
        if (out != plain_out) {
            differs++;
        }
    }

    check(nonzero > 0, "the graph renders a non-silent carrier");

    /* The modulated voice must differ from the unmodulated one: the
     * modulation enters the carrier's phase in the same sample. */
    check(differs > 900,
          "the modulator's output reaches the carrier in the same sample");

    /* The same voice rendered twice must give the same samples. */
    {
        slopsix_voice_engine again;

        build_voice(&again, 0, 60, 4095);
        for (i = 0; i < 1024; i++) {
            int32_t out;

            slopsix_voice_render_sample(&again,
                                        slopsix_algorithm_get(0),
                                        &out);
            if (out != samples[i]) {
                deterministic = 0;
            }
        }
    }
    check(deterministic, "the graph walk is deterministic");

    /* An algorithm whose carrier is modulated through the sum/delay
     * register must render too. */
    {
        slopsix_voice_engine other;
        int32_t out;
        int32_t sum = 0;

        build_voice(&other, 6, 60, 4095);   /* algorithm 7 */
        for (i = 0; i < 256; i++) {
            slopsix_voice_render_sample(&other,
                                        slopsix_algorithm_get(6),
                                        &out);
            sum += out;
        }
        check(sum != 0,
              "an algorithm with a sum/delay register renders a signal");
    }
}

/* ------------------------------------------------- Phase 2: the algorithm */

/* The decoded carrier sets and feedback operators of the 32 algorithms
 * (research A-01: 32/32 against the service-manual chart and
 * 32/32 against the analysed engine's feedback structure). */
static const uint8_t dx7_carriers[32][6] = {
    { 1, 3, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 }, { 1, 4, 0, 0, 0, 0 },
    { 1, 4, 0, 0, 0, 0 }, { 1, 3, 5, 0, 0, 0 }, { 1, 3, 5, 0, 0, 0 },
    { 1, 3, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 },
    { 1, 4, 0, 0, 0, 0 }, { 1, 4, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 },
    { 1, 3, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 }, { 1, 3, 0, 0, 0, 0 },
    { 1, 0, 0, 0, 0, 0 }, { 1, 0, 0, 0, 0, 0 }, { 1, 0, 0, 0, 0, 0 },
    { 1, 4, 5, 0, 0, 0 }, { 1, 2, 4, 0, 0, 0 }, { 1, 2, 4, 5, 0, 0 },
    { 1, 3, 4, 5, 0, 0 }, { 1, 2, 4, 5, 0, 0 }, { 1, 2, 3, 4, 5, 0 },
    { 1, 2, 3, 4, 5, 0 }, { 1, 2, 4, 0, 0, 0 }, { 1, 2, 4, 0, 0, 0 },
    { 1, 3, 6, 0, 0, 0 }, { 1, 2, 3, 5, 0, 0 }, { 1, 2, 3, 6, 0, 0 },
    { 1, 2, 3, 4, 5, 0 }, { 1, 2, 3, 4, 5, 6 }
};

static const uint8_t dx7_feedback_op[32] = {
    6, 2, 6, 6, 6, 6, 6, 4, 2, 3, 6, 2, 6, 6, 2, 6,
    2, 3, 6, 3, 3, 6, 6, 6, 6, 6, 3, 5, 6, 5, 6, 6
};

static void stage_algorithms(void)
{
    unsigned alg;
    int carriers_ok = 1;
    int feedback_ok = 1;
    int order_ok = 1;
    int source_ok = 1;

    printf("stage algorithms\n");

    for (alg = 0; alg < 32u; alg++) {
        const slopsix_algorithm *decoded = slopsix_algorithm_get(alg);
        unsigned op;

        /* The carrier set. */
        for (op = 1; op <= SLOPSIX_OP_COUNT; op++) {
            int want = 0;
            unsigned i;

            for (i = 0; i < 6u; i++) {
                if (dx7_carriers[alg][i] == (uint8_t)op) {
                    want = 1;
                }
            }
            if ((decoded->ops[op - 1u].carrier != 0u) != (want != 0)) {
                carriers_ok = 0;
            }
        }

        /* The feedback operator. */
        if (slopsix_algorithm_feedback_operator(alg) !=
            dx7_feedback_op[alg]) {
            feedback_ok = 0;
        }

        /* An operator is only ever modulated by a higher-numbered
         * operator, except for its own feedback path.  The selector is
         * stored on the row that produces it; that row's consumer is the
         * previous operator (the wrap column's consumer is operator 6). */
        for (op = 1; op <= SLOPSIX_OP_COUNT; op++) {
            unsigned consumer = (op == 1u) ? SLOPSIX_OP_COUNT : (op - 1u);

            if (decoded->ops[op - 1u].source == SLOPSIX_MOD_OPERATOR &&
                decoded->ops[op - 1u].source_op <= (uint8_t)consumer) {
                order_ok = 0;
            }
        }
    }

    check(carriers_ok, "all 32 algorithms decode the documented carriers");
    check(feedback_ok,
          "all 32 algorithms decode the documented feedback operator");
    check(order_ok,
          "no operator is modulated by an equal or lower-numbered one");

    /* Algorithm 18's selectors differ from row to row, so it pins the
     * producer-row placement: op6 SEL1, op5 SEL1, op4 SEL5, op3 SEL0,
     * op2 SEL2, op1 SEL0. */
    {
        const slopsix_algorithm *a = slopsix_algorithm_get(17u);
        static const slopsix_mod_source want[6] = {
            SLOPSIX_MOD_NONE, SLOPSIX_MOD_REGISTER, SLOPSIX_MOD_NONE,
            SLOPSIX_MOD_FEEDBACK, SLOPSIX_MOD_OPERATOR,
            SLOPSIX_MOD_OPERATOR
        };
        unsigned op;

        for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
            if (a->ops[op].source != want[op]) {
                source_ok = 0;
            }
        }
    }
    check(source_ok, "each selector is stored on its producer row");
}

/* ------------------------------------------- Phase 3: the envelope and the
 * level pipeline */

/* A representative operator level for the envelope's tests: the output
 * level at its maximum and a middle velocity. */
#define TEST_OP_LEVEL_Q8 3328

/* The envelope's target level, as the engine composes it. */
static int32_t test_eg_target(uint8_t level_param, int32_t op_level_q8)
{
    int32_t target = ((slopsix_level_curve(level_param) >> 1) << 6) +
                     op_level_q8 - SLOPSIX_LEVEL_OFFSET_Q8;

    if (target < SLOPSIX_EG_MIN_Q8) {
        target = SLOPSIX_EG_MIN_Q8;
    }
    return target;
}

static void stage_eg(void)
{
    slopsix_eg eg;
    uint8_t rates[4] = { 99, 99, 99, 99 };
    uint8_t levels[4] = { 99, 99, 99, 0 };
    int32_t target;
    int32_t level;
    int i;

    printf("stage eg\n");

    /* The level curve: the hardware's 0..99 parameter mapped onto the
     * engine's 0..127 microstep gain. */
    check(slopsix_level_curve(0) == 0, "level_curve(0) == 0");
    check(slopsix_level_curve(19) == 46, "level_curve(19) == 46");
    check(slopsix_level_curve(20) == 48, "level_curve(20) == 48");
    check(slopsix_level_curve(99) == 127, "level_curve(99) == 127");

    /* The attack: the envelope rises to its target and holds it. */
    slopsix_eg_init(&eg, rates, levels, TEST_OP_LEVEL_Q8, 0,
                    SLOPSIX_DEFAULT_SAMPLE_RATE);
    target = test_eg_target(99, TEST_OP_LEVEL_Q8);

    level = 0;
    for (i = 0; i < 4000; i++) {
        level = slopsix_eg_step(&eg);
        if (level == target) {
            break;
        }
    }
    check(level == target, "the attack reaches the stage's target level");
    check(i < 4000, "the attack reaches its target in finite time");

    /* The decay: the envelope falls from the first target to the
     * second one. */
    {
        int32_t decay_target = test_eg_target(99, TEST_OP_LEVEL_Q8);
        int reached = 0;

        for (i = 0; i < 4000; i++) {
            level = slopsix_eg_step(&eg);
            if (level == decay_target && eg.stage >= 2u) {
                reached = 1;
                break;
            }
        }
        check(reached, "the decay reaches the sustain stage");
    }

    /* The release: the key up jumps to the release stage. */
    slopsix_eg_note_off(&eg);
    check(eg.stage == 3u, "the key up jumps to the release stage");
    level = slopsix_eg_step(&eg);
    check(level <= target, "the release falls from the sustain level");

    /* The rate scaling: a higher note makes the envelope faster. */
    {
        slopsix_eg low;
        slopsix_eg high;
        uint8_t middle[4] = { 50, 50, 50, 50 };
        int32_t low_level = 0;
        int32_t high_level = 0;
        int low_samples = 0;
        int high_samples = 0;

        slopsix_eg_init(&low, middle, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        slopsix_eg_init(&high, middle, levels, TEST_OP_LEVEL_Q8,
                        slopsix_rate_scaling(96, 7),
                        SLOPSIX_DEFAULT_SAMPLE_RATE);

        for (i = 0; i < 10000; i++) {
            low_level = slopsix_eg_step(&low);
            high_level = slopsix_eg_step(&high);
            if (low_samples == 0 && low_level == target) {
                low_samples = i + 1;
            }
            if (high_samples == 0 && high_level == target) {
                high_samples = i + 1;
            }
        }

        check(slopsix_rate_scaling(96, 7) > 0,
              "the rate scaling grows with the note");
        check(slopsix_rate_scaling(0, 7) == 0,
              "the rate scaling is zero at the bottom of the keyboard");
        check(high_samples > 0 && low_samples > 0 &&
              high_samples < low_samples,
              "the rate scaling makes the envelope faster");
    }

    /* The increment's saturation: the top four q values (60..63, from
     * patch rates 95..99) share the fastest step, while a rate just
     * below still has a smaller increment. */
    {
        slopsix_eg fast;
        slopsix_eg top;
        slopsix_eg mid;
        uint8_t r91[4] = { 91, 91, 91, 91 };
        uint8_t r95[4] = { 95, 95, 95, 95 };
        uint8_t r96[4] = { 96, 96, 96, 96 };
        uint8_t r97[4] = { 97, 97, 97, 97 };
        uint8_t r99[4] = { 99, 99, 99, 99 };

        slopsix_eg_init(&fast, r95, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        slopsix_eg_init(&mid, r91, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);

        slopsix_eg_init(&top, r96, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        check(top.inc == fast.inc,
              "rates 96 and 95 share the fastest increment");
        slopsix_eg_init(&top, r97, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        check(top.inc == fast.inc,
              "rates 97 and 95 share the fastest increment");
        slopsix_eg_init(&top, r99, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        check(top.inc == fast.inc,
              "rates 99 and 95 share the fastest increment");
        check(mid.inc < fast.inc,
              "a rate below the saturation point is still slower");
    }

    /* The attack out of silence: the level jumps to the attack's
     * starting point. */
    {
        uint8_t silent[4] = { 0, 99, 99, 0 };
        int32_t jump = 0;

        slopsix_eg_init(&eg, rates, silent, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);

        for (i = 0; i < 64; i++) {
            level = slopsix_eg_step(&eg);
            if (level > jump) {
                jump = level;
            }
        }
        check(jump > 0, "the attack out of silence jumps to its start point");
    }

    /* A stage whose target is the current level waits for its measured
     * duration instead of moving. */
    {
        uint8_t equal[4] = { 99, 99, 99, 0 };
        int held = 0;

        slopsix_eg_init(&eg, rates, equal, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);

        /* The first stage's target equals the second one's, so the
         * envelope holds the level while the static count runs. */
        for (i = 0; i < 64; i++) {
            level = slopsix_eg_step(&eg);
            if (level == target) {
                held++;
            }
        }
        check(held > 0,
              "a stage with a reached target holds the level for its "
              "measured duration");
    }

    /* BANJO's carrier holds L1 == L2 on stage 1.  The EGS advances such
     * a held stage immediately; only the sustain/release stages hold for
     * their measured duration. */
    {
        uint8_t banjo_rates[4] = { 98, 36, 44, 56 };
        uint8_t banjo_levels[4] = { 99, 99, 0, 0 };
        int steps = 0;

        slopsix_eg_init(&eg, banjo_rates, banjo_levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        while (eg.stage < 2u && steps < 4096) {
            level = slopsix_eg_step(&eg);
            steps++;
        }
        check(steps < 4096,
              "an equal-target held stage advances without the static hold");
    }

    /* The two cadences agree in timing: the increment is a block's worth
     * and the per-sample cadence divides it by the block size. */
    {
        slopsix_eg per_sample;
        slopsix_eg per_block;
        int sample_reached = -1;
        int block_reached = -1;

        slopsix_eg_init(&per_sample, rates, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        slopsix_eg_init(&per_block, rates, levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        per_block.block_mode = 1u;

        for (i = 0; i < 200000; i++) {
            if (sample_reached < 0 &&
                slopsix_eg_step(&per_sample) == target) {
                sample_reached = i;
            }
            if (block_reached < 0 &&
                slopsix_eg_step(&per_block) == target) {
                block_reached = i;
            }
            if (sample_reached >= 0 && block_reached >= 0) {
                break;
            }
        }

        check(sample_reached > 0 && block_reached > 0,
              "both cadences reach the target");
        check(abs(sample_reached - block_reached) <= SLOPSIX_EG_BLOCK,
              "the two cadences agree in timing (within a block)");
    }

    /* The envelope's block cadence: the envelope advances once per block
     * and the level is interpolated across it. */
    {
        uint8_t block_levels[4] = { 99, 99, 99, 0 };
        int32_t previous;
        int monotonic = 1;

        slopsix_eg_init(&eg, rates, block_levels, TEST_OP_LEVEL_Q8, 0,
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        eg.block_mode = 1u;

        previous = slopsix_eg_step(&eg);
        for (i = 0; i < 1024; i++) {
            level = slopsix_eg_step(&eg);
            if (level > target) {
                monotonic = 0;
            }
            if (level < previous && previous != target) {
                monotonic = 0;
            }
            previous = level;
        }
        check(monotonic,
              "the block cadence interpolates the level towards the target");
        check(level == target, "the block cadence reaches the target too");
    }
}

static void stage_level(void)
{
    int8_t offsets[SLOPSIX_KLS_GROUPS];
    int i;

    printf("stage level\n");

    /* The keyboard level scaling: the curve is built per key-code group
     * and read at the note's group. */
    {
        uint8_t breakpoint = 39;   /* the hardware's C3 */
        uint8_t group;
        int at_breakpoint = 0;
        int negative_above = 0;
        int grows = 0;

        /* A negative right curve: the level falls above the
         * breakpoint. */
        slopsix_kls_build(offsets, breakpoint, 0, 99, 0, 0);
        group = slopsix_note_group(72);
        if (offsets[group] < 0) {
            negative_above = 1;
        }
        if (offsets[slopsix_kls_breakpoint_index(breakpoint) - 1u] == 0) {
            at_breakpoint = 1;
        }
        if (offsets[slopsix_note_group(96)] < offsets[group]) {
            grows = 1;
        }

        check(at_breakpoint,
              "the keyboard level scaling is neutral at the breakpoint");
        check(negative_above,
              "a negative curve lowers the level above the breakpoint");
        check(grows,
              "the keyboard level scaling grows with the distance");

        /* A positive right curve: the level rises above the
         * breakpoint. */
        slopsix_kls_build(offsets, breakpoint, 0, 99, 0, 3);
        check(offsets[slopsix_note_group(96)] > 0,
              "a positive curve raises the level above the breakpoint");

        /* The curve's magnitude saturates at the table's top. */
        slopsix_kls_build(offsets, breakpoint, 0, 99, 0, 0);
        check(offsets[slopsix_note_group(127)] == -SLOPSIX_MICROSTEP_MAX &&
              offsets[slopsix_note_group(96)] > offsets[slopsix_note_group(127)],
              "the keyboard level scaling saturates at the table's top");
    }

    /* The velocity route: a harder key is louder. */
    {
        int32_t soft = slopsix_op_level_q8(99, 0, 20, 7);
        int32_t hard = slopsix_op_level_q8(99, 0, 120, 7);
        int32_t soft_none = slopsix_op_level_q8(99, 0, 20, 0);
        int32_t hard_none = slopsix_op_level_q8(99, 0, 120, 0);

        check(hard > soft, "a harder key gives a louder operator");
        check(soft_none == hard_none,
              "a velocity sensitivity of zero ignores the velocity");
        check(slopsix_velocity_attenuation(120, 7) <
              slopsix_velocity_attenuation(20, 7),
              "the velocity route's attenuation falls with the velocity");
    }

    /* The operator output level. */
    {
        int32_t full = slopsix_op_level_q8(99, 0, 127, 0);
        int32_t none = slopsix_op_level_q8(0, 0, 127, 0);

        check(full > none, "the operator output level raises the level");
    }

    /* The amplitude modulation: the EGS form is an attenuation addition,
     * `a += amp_mod << ams` (the register value times the sensitivity's
     * shift); in the engine's gain direction this file checks the same
     * quantity as the subtraction the walk applies.  The register value
     * arrives from the LFO's per-tick composition. */
    check(slopsix_ams_reduction_q8(0, 16) == 0,
          "an operator without amplitude modulation is not affected");
    check(slopsix_ams_reduction_q8(3, 16) == 16 << 3,
          "the amplitude modulation lowers the level by the shifted amount");
    check(slopsix_ams_reduction_q8(3, 0) == 0,
          "the amplitude modulation does nothing with an empty register");
    check(slopsix_ams_reduction_q8(1, 16) < slopsix_ams_reduction_q8(3, 16),
          "the amplitude modulation's depth grows with the sensitivity");

    /* The carrier-count compensation: every carrier of an algorithm is
     * divided by the algorithm's carrier count, so that a voice sounds
     * equally loud in every algorithm. */
    {
        unsigned alg;
        int compensation_ok = 1;

        for (alg = 0; alg < 32u; alg++) {
            const slopsix_algorithm *decoded = slopsix_algorithm_get(alg);
            unsigned carriers = 0;
            unsigned op;
            int32_t total = 0;

            for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
                if (decoded->ops[op].carrier) {
                    carriers++;
                    total += slopsix_com_q8(decoded->ops[op].com);
                }
            }

            /* Every carrier of an algorithm carries the algorithm's
             * carrier count minus one, and the compensation is the
             * hardware's six-step approximation of log2 of the carrier
             * count. */
            if (carriers > 0u) {
                /* The ideal compensation is log2 of the carrier count,
                 * in the Q8 domain: 256 units per octave. */
                static const int32_t ideal[6] = {
                    0, 256, 406, 512, 594, 662
                };
                int32_t per_carrier = -slopsix_com_q8(
                    (uint8_t)(carriers - 1u));

                for (op = 0; op < SLOPSIX_OP_COUNT; op++) {
                    if (decoded->ops[op].carrier &&
                        decoded->ops[op].com != (uint8_t)(carriers - 1u)) {
                        compensation_ok = 0;
                    }
                }

                /* The hardware's six-step table approximates log2 of
                 * the carrier count to within 0.16 of an octave. */
                if (per_carrier < ideal[carriers - 1u] - 40 ||
                    per_carrier > ideal[carriers - 1u] + 40) {
                    compensation_ok = 0;
                }
            }
        }

        check(compensation_ok,
              "every algorithm's carriers carry the carrier-count "
              "compensation");
    }

    /* The compensation's value: it is log2 of the carrier count, in the
     * Q8 domain. */
    check(slopsix_com_q8(0) == 0, "a single carrier is not compensated");
    check(slopsix_com_q8(1) == -256, "two carriers are compensated by 2");
    check(slopsix_com_q8(5) == -672, "six carriers are compensated by 6");

    /* The level composition reaches the render path: the envelope's
     * level is the operator's log amplitude. */
    {
        slopsix_voice_engine voice;
        uint8_t rates[4] = { 99, 99, 99, 99 };
        uint8_t levels[4] = { 99, 99, 99, 0 };
        int32_t out = 0;
        int nonzero = 0;

        memset(&voice, 0, sizeof(voice));
        for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
            voice.params[i].mode = 0;
            voice.params[i].coarse = 1;
            voice.params[i].fine = 0;
            voice.params[i].detune = 7;
            voice.ops[i].increment = slopsix_increment_from_word(
                slopsix_op_freq_word(60, &voice.params[i], 0, 0),
                SLOPSIX_DEFAULT_SAMPLE_RATE);
            slopsix_eg_init(&voice.eg[i], rates, levels, TEST_OP_LEVEL_Q8, 0,
                            SLOPSIX_DEFAULT_SAMPLE_RATE);
        }

        for (i = 0; i < 512; i++) {
            slopsix_voice_render_sample(&voice, slopsix_algorithm_get(0),
                                        &out);
            if (out != 0) {
                nonzero++;
            }
        }
        check(nonzero > 0, "the envelope drives the rendered voice");
    }
}

/* The loudness equality across algorithms: the carrier-count
 * compensation divides every carrier by the algorithm's carrier count,
 * so a voice with the same per-carrier level sounds as loud in a
 * six-carrier algorithm as in a single-carrier one. */
static void stage_com(void)
{
    slopsix_voice_engine voice;
    int32_t peak_many = 0;
    int32_t peak_one = 0;
    int i;

    printf("stage com\n");

    /* Algorithm 32: six carriers, every one of them compensated by six. */
    memset(&voice, 0, sizeof(voice));
    for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
        voice.params[i].mode = 0;
        voice.params[i].coarse = 1;
        voice.params[i].fine = 0;
        voice.params[i].detune = 7;
        voice.ops[i].increment = slopsix_increment_from_word(
            slopsix_op_freq_word(60, &voice.params[i], 0, 0),
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        voice.params[i].log_amp_q10 = 4000 << SLOPSIX_AMP_Q8_SHIFT;
    }
    for (i = 0; i < 512; i++) {
        int32_t out = 0;

        slopsix_voice_render_sample(&voice, slopsix_algorithm_get(31),
                                    &out);
        if (out > peak_many) {
            peak_many = out;
        }
    }

    /* Algorithm 16: a single carrier, not compensated at all. */
    memset(&voice, 0, sizeof(voice));
    for (i = 0; i < SLOPSIX_OP_COUNT; i++) {
        voice.params[i].mode = 0;
        voice.params[i].coarse = 1;
        voice.params[i].fine = 0;
        voice.params[i].detune = 7;
        voice.ops[i].increment = slopsix_increment_from_word(
            slopsix_op_freq_word(60, &voice.params[i], 0, 0),
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        voice.params[i].log_amp_q10 = (i == 0)
            ? (4000 << SLOPSIX_AMP_Q8_SHIFT) : 0;
    }
    for (i = 0; i < 512; i++) {
        int32_t out = 0;

        slopsix_voice_render_sample(&voice, slopsix_algorithm_get(15),
                                    &out);
        if (out > peak_one) {
            peak_one = out;
        }
    }

    check(peak_many > 0 && peak_one > 0,
          "both algorithms render a signal");
    check(peak_many * 100 >= peak_one * 95 &&
          peak_many * 100 <= peak_one * 105,
          "a six-carrier algorithm is as loud as a single-carrier one");
}

/* --------------------------------------------------- the pitch bend */

static void stage_bend(void)
{
    slopsix_context ctx;
    int32_t value;
    int32_t down;
    unsigned i;
    int seen_41 = 0;
    int seen_87 = 0;

    printf("stage bend\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }

    /* The centre of the bend is the neutral one. */
    slopsix_set_pitch_bend(ctx, 0x2000u);
    slopsix_set_pitch_bend_range(ctx, 12u, 0u);
    check(slopsix_bend_pitch_mod(0x2000u, 12u, 0u) == 0,
          "the centred bend is neutral");

    /* The dead zone: the curve holds the neutral value for the three
     * input steps around the centre. */
    check(slopsix_bend_pitch_mod(8064u, 11u, 0u) == 0,
          "the bend's dead zone starts one step below the centre");
    check(slopsix_bend_pitch_mod(8320u, 11u, 0u) == 0,
          "the bend's dead zone ends one step above the centre");
    check(slopsix_bend_pitch_mod(8448u, 11u, 0u) != 0,
          "the bend leaves the dead zone one step further out");

    /* The input is reduced to the hardware's seven bits. */
    check(slopsix_bend_pitch_mod(0x2000u, 11u, 0u) ==
          slopsix_bend_pitch_mod(0x207Fu, 11u, 0u),
          "the bend is quantised to the hardware's seven bits");

    /* The continuous path: the range scales the bend, and the full
     * deflection reaches the range's semitones (within the input's
     * quantisation). */
    value = slopsix_bend_pitch_mod(0x3FFFu, 11u, 0u);
    check(value == 14868, "the full upwards bend follows the range's scale");
    check(value * 100 > 11 * 1365 * 98 && value * 100 < 11 * 1365 * 100,
          "the full upwards bend reaches the range's semitones");

    /* The down side is one step short of the up side (the |s|-1
     * compression) and reaches one step further. */
    down = slopsix_bend_pitch_mod(0u, 11u, 0u);
    check(down == -14987, "the full downwards bend follows the |s|-1 law");
    check(-down > value, "the downwards travel reaches further than the up");

    check(slopsix_bend_pitch_mod(7936u, 11u, 0u) == -119,
          "the first step below the dead zone is compressed");
    check(slopsix_bend_pitch_mod(8448u, 11u, 0u) == 236,
          "the first step above the dead zone is not compressed");

    /* Two of the curve's values are never produced by any input.  Value 41
     * is on the down side, so it surfaces as the negative word. */
    for (i = 0; i < 128u; i++) {
        int32_t bend = (int32_t)((unsigned)i << 7);
        if (slopsix_bend_pitch_mod((uint16_t)bend, 11u, 0u) == -5311) {
            seen_41 = 1;
        }
        if (slopsix_bend_pitch_mod((uint16_t)bend, 11u, 0u) == 5428) {
            seen_87 = 1;
        }
    }
    check(!seen_41, "the curve's value 41 is never produced");
    check(!seen_87, "the curve's value 87 is never produced");

    /* The range-12 path has its own scale, with the top sample special:
     * the top sample's word is halved into 0x3FFF. */
    check(slopsix_bend_pitch_mod(0x3FFFu, 12u, 0u) == 16383,
          "the range-12 top sample is the halved word");
    check(slopsix_bend_pitch_mod(16128u, 12u, 0u) == 15872,
          "the range-12 path has its own scale");
    check(slopsix_bend_pitch_mod(0u, 12u, 0u) == -16384,
          "the range-12 downwards bend reaches the full travel");

    /* A non-zero step ignores the range and quantises the bend into the
     * step table's staircase. */
    check(slopsix_bend_pitch_mod(0x3FFFu, 0u, 3u) ==
          slopsix_bend_pitch_mod(0x3FFFu, 12u, 3u),
          "the step ignores the range");
    check(slopsix_bend_pitch_mod(0x3FFFu, 11u, 3u) == 16384,
          "the step's staircase reaches the full travel");
    check(slopsix_bend_pitch_mod(0u, 11u, 3u) == -16385,
          "the step's downwards staircase is the complement");
    check(slopsix_bend_pitch_mod(0x3FFFu, 11u, 13u) == 0 &&
          slopsix_bend_pitch_mod(0u, 11u, 14u) == 0 &&
          slopsix_bend_pitch_mod(0x2000u, 11u, 15u) == 0,
          "the step table's zero groups carry no bend");

    /* The engine's word: the pitch mod is four times finer than the
     * engine's log-frequency word, so it is shifted right by two. */
    check(slopsix_bend_log_offset(0x3FFFu, 11u, 0u) == (14868 >> 2),
          "the bend enters the engine's word shifted by two");

    /* The public setters reach the engine's bend parameters. */
    slopsix_set_pitch_bend(ctx, 0x3FFFu);
    slopsix_set_pitch_bend_range(ctx, 11u, 0u);
    check(SLOPSIX_CONTEXT(ctx)->pitch_bend == 0x3FFFu &&
          SLOPSIX_CONTEXT(ctx)->bend_range == 11u &&
          SLOPSIX_CONTEXT(ctx)->bend_step == 0u,
          "the public setters store the bend's parameters");
    check(slopsix_bend_context_offset(SLOPSIX_CONTEXT(ctx)) ==
          slopsix_bend_log_offset(0x3FFFu, 11u, 0u),
          "the stored bend produces the engine's offset");

    slopsix_destroy_context(ctx);
}

/* -------------------------------------------------- the pitch EG */

static void stage_pitcheg(void)
{
    slopsix_pitch_eg eg;
    slopsix_pitch_eg_cadence cadence;
    slopsix_op_params params;
    uint8_t rates[4];
    uint8_t levels[4];
    int intervals[3];
    int updates;
    int previous;
    int i;
    int32_t neutral;

    printf("stage pitcheg\n");

    /* The neutral level is 50 in the patch's units: its table entry is
     * 0x80 and its 16-bit word is the firmware's 0x4000. */
    rates[0] = rates[1] = rates[2] = rates[3] = 0u;
    levels[0] = levels[1] = levels[2] = levels[3] = 50u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    neutral = slopsix_pitch_eg_offset(&eg);
    check(neutral == 0, "level 50 is the neutral pitch-EG level");

    /* The level table's endpoints and nonlinear top.  The table value
     * becomes the level word by a left shift of seven, and the offset is
     * the word's distance from 0x4000. */
    levels[0] = levels[1] = levels[2] = levels[3] = 0u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    check(slopsix_pitch_eg_offset(&eg) == -16384,
          "level 0 is four octaves below neutral");

    levels[0] = levels[1] = levels[2] = levels[3] = 99u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 16256,
          "level 99 is just under four octaves above neutral");

    levels[0] = levels[1] = levels[2] = levels[3] = 51u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 128,
          "level 51 is one table step above neutral");

    levels[0] = levels[1] = levels[2] = levels[3] = 86u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 4864,
          "level 86 follows the level table's nonlinear top");

    /* The state walk.  The envelope starts at the release level (L4),
     * walks L1, L2 and L3, holds there, and on key-off releases to L4
     * again.  The full swing at rate 99 moves 255 units per update. */
    rates[0] = rates[1] = rates[2] = rates[3] = 99u;
    levels[0] = 99u;
    levels[1] = 50u;
    levels[2] = 50u;
    levels[3] = 0u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    check(slopsix_pitch_eg_offset(&eg) == -16384,
          "key-on starts at the final (release) level");

    /* 0x0000 -> 0x7F00 is 128 increments of 255. */
    for (i = 0; i < 127; i++) {
        slopsix_pitch_eg_update(&eg);
    }
    check(slopsix_pitch_eg_offset(&eg) == 16001 && eg.step == 0u,
          "the first stage has not finished one step early");
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 16256 && eg.step == 1u,
          "the first stage snaps to its level and advances");

    /* 0x7F00 -> 0x4000 is 63 increments of 255 (the last one snaps). */
    for (i = 0; i < 62; i++) {
        slopsix_pitch_eg_update(&eg);
    }
    check(slopsix_pitch_eg_offset(&eg) == 446 && eg.step == 1u,
          "the second stage is still moving after 62 steps");
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 0 && eg.step == 2u,
          "the second stage reaches neutral and advances");

    /* L3 is also neutral, so the sustain is reached on the next tick. */
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 0 && eg.step == 3u,
          "the sustain holds the neutral level");

    /* The sustain ignores further updates until key-off. */
    for (i = 0; i < 100; i++) {
        slopsix_pitch_eg_update(&eg);
    }
    check(slopsix_pitch_eg_offset(&eg) == 0 && eg.step == 3u,
          "the held sustain stops the envelope");

    /* Key-off releases to L4 from the current level; the release needs
     * 64 increments of 255 from 0x4000 to 0x0000. */
    slopsix_pitch_eg_note_off(&eg);
    check(eg.step == 4u, "key-off enters the release stage");
    for (i = 0; i < 63; i++) {
        slopsix_pitch_eg_update(&eg);
    }
    check(slopsix_pitch_eg_offset(&eg) == -16065 && eg.step == 4u,
          "the release is still moving after 63 steps");
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == -16384 && eg.step == 5u,
          "the release reaches the final level and finishes");

    /* The firmware snaps a stage at the target's high byte, so a short
     * move that crosses that byte finishes in one update, not at the
     * exact target distance. */
    rates[0] = 99u;
    levels[0] = 51u;
    levels[1] = 51u;
    levels[2] = 51u;
    levels[3] = 50u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 128 && eg.step == 1u,
          "the high byte snaps a short rise to the target");

    /* A stage whose target already equals the level advances instantly. */
    levels[0] = 50u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    slopsix_pitch_eg_update(&eg);
    check(slopsix_pitch_eg_offset(&eg) == 0 && eg.step == 1u,
          "an equal target advances the stage");

    /* The cadence: at the anchor rate the firmware's every-second-tick
     * process is 785 / 3 samples, an interval of 262, 262, 261.  The
     * gate is global, so it lives outside the envelope: it reports the
     * samples on which every voice's envelope advances. */
    rates[0] = rates[1] = rates[2] = rates[3] = 0u;
    levels[0] = levels[1] = levels[2] = levels[3] = 50u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_cadence_init(&cadence, SLOPSIX_DEFAULT_SAMPLE_RATE);
    updates = 0;
    previous = 0;
    for (i = 1; i <= 1000; i++) {
        if (slopsix_pitch_eg_cadence_tick(&cadence)) {
            if (updates < 3) {
                intervals[updates] = i - previous;
            }
            previous = i;
            updates++;
        }
    }
    check(updates >= 3 && intervals[0] == 262 && intervals[1] == 262 &&
          intervals[2] == 261,
          "the pitch EG updates at the 187.6 Hz staircase");

    /* The gate advances only the cadence: the envelope's level is
     * unchanged by it, so the two cannot step an envelope twice. */
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    slopsix_pitch_eg_cadence_init(&cadence, SLOPSIX_DEFAULT_SAMPLE_RATE);
    for (i = 1; i <= 1000; i++) {
        if (slopsix_pitch_eg_cadence_tick(&cadence)) {
            break;
        }
    }
    check(slopsix_pitch_eg_offset(&eg) == 0 && eg.step == 0u,
          "the cadence gate does not step the envelope itself");

    /* A host rate scales the update's real-time period: at twice the
     * anchor rate the first interval is twice the anchor's. */
    slopsix_pitch_eg_cadence_init(&cadence,
                                  SLOPSIX_DEFAULT_SAMPLE_RATE * 2u);
    for (i = 1; i <= 2000; i++) {
        if (slopsix_pitch_eg_cadence_tick(&cadence)) {
            break;
        }
    }
    check(i == 524, "the cadence scales with the host sample rate");

    /* The frequency path takes the pitch EG's offset in the same word
     * as the note, and the fixed-frequency mode ignores it. */
    params.mode = 0u;
    params.coarse = 1u;
    params.fine = 0u;
    params.detune = 7u;
    check(slopsix_op_freq_word(69, &params, 0, 4096) ==
          slopsix_op_freq_word(69, &params, 0, 0) + 4096,
          "the pitch-EG offset is one octave per 4096 word units");
    params.mode = 1u;
    check(slopsix_op_freq_word(69, &params, 0, 4096) ==
          slopsix_op_freq_word(69, &params, 0, 0),
          "a fixed-frequency operator ignores the pitch EG");

    /* A sweep: from L4 = 0 (-4 octaves) to L1 = 99 (+3.97 octaves) the
     * rendered frequency rises by almost eight octaves, monotonically. */
    params.mode = 0u;
    rates[0] = rates[1] = rates[2] = rates[3] = 99u;
    levels[0] = levels[1] = levels[2] = levels[3] = 0u;
    levels[0] = 99u;
    slopsix_pitch_eg_init(&eg, rates, levels);
    slopsix_pitch_eg_note_on(&eg);
    {
        int64_t previous = 0;
        int64_t first = 0;
        int64_t last = 0;
        int monotonic = 1;

        for (i = 0; i <= 128; i++) {
            int32_t word = slopsix_op_freq_word(69, &params, 0,
                                                slopsix_pitch_eg_offset(&eg));
            uint32_t increment = slopsix_increment_from_word(
                word, SLOPSIX_DEFAULT_SAMPLE_RATE);
            int64_t freq = increment_millihertz(increment,
                                                SLOPSIX_DEFAULT_SAMPLE_RATE);

            if (i == 0) {
                first = freq;
            }
            if (freq < previous) {
                monotonic = 0;
            }
            previous = freq;
            last = freq;
            slopsix_pitch_eg_update(&eg);
        }

        check(monotonic, "the pitch-EG sweep rises monotonically");
        check(first > 0 && last * 100 > first * 24000 &&
              last * 100 < first * 26000,
              "the sweep rises by almost eight octaves");
    }
}

/* --------------------------------------------- Phase 5: the output stages
 *
 * The reference forms live in this stage: the compander's shift
 * thresholds and clamps, the DAC step's grid and rails and the master
 * ladder's tap gains are written out here so the library is checked
 * against the documented recipes and not against itself. */

static void compander_expect(int32_t sample, int32_t mantissa,
                             unsigned shift, const char *what)
{
    slopsix_compander_word w;

    slopsix_compander_quantize(sample, &w);
    check(w.mantissa == mantissa && w.shift == shift, what);
    if (w.mantissa == mantissa && w.shift == shift) {
        check(slopsix_compander_value(&w) ==
              (int32_t)mantissa << shift, "the word's value is mantissa << shift");
    }
}

/* The output stage's filter is stateful: a constant bus has to run
 * through it before its steady state can be read.  The filter's DC gain
 * is one, so a settled constant lands on the code's own value. */
static void output_settle(slopsix_context_state *state, int32_t bus,
                          int16_t *left, int16_t *right)
{
    int i;

    for (i = 0; i < 4096; i++) {
        slopsix_output_mix(state, bus, left, right);
    }
}

/* The closeness of a measured value to its expectation: the filter's
 * settled value carries the fixed point's last bits, so the output
 * stages' exact values are read with a small tolerance. */
static int output_near(int16_t got, int expect, int tolerance)
{
    int delta = (int)got - expect;

    if (delta < 0) {
        delta = -delta;
    }
    return delta <= tolerance;
}

/* ------------------------------------------- the analog tail's filter
 *
 * The reconstruction filter: the prototype's coefficients at the anchor
 * rate, the DC gain, and the response at the reference's frequencies.
 * The response is measured through the filter itself - a sine runs
 * through it and the steady-state amplitude ratio is read back - so the
 * check is on the engine's own arithmetic and not on the coefficients
 * alone.
 */

/* The prototype, as the generator emits it: the base research fit rescaled
 * to the DX7 tail's measured 17.02 kHz corner. */
static const int32_t filter_prototype[3][5] = {
    { 535692986, 535692986, 0, -2355853, 0 },
    { 611162297, 1222324595, 611162297, 763595718, 607311647 },
    { 953404960, 1906809919, 953404960, 1795222135, 944655879 }
};

/* A sine wave: the phase runs in 32 bits over a whole period and the
 * wave is the polynomial sine on a quarter wave, in Q15.
 *
 *   sin(x) ~ x - x^3/6 + x^5/120 - x^7/5040
 *
 * The argument is at most pi/2, where the truncation leaves about 1.6e-4
 * of the amplitude - far below the measurement's own tolerance. */
static int32_t harness_sin(uint32_t phase)
{
    uint32_t quadrant = (phase >> 30) & 3u;
    uint32_t index = phase & 0x3FFFFFFFu;
    int64_t x;
    int64_t x2;
    int64_t x3;
    int64_t x5;
    int64_t x7;
    int32_t value;

    if ((quadrant & 1u) != 0u) {
        /* The second and the fourth quadrant mirror the wave. */
        index = (1u << 30) - 1u - index;
    }

    /* The argument inside the quarter wave, in Q15 (pi/2 is 51472).  The
     * powers are 64-bit: a Q15 product of two quarter-wave arguments
     * overflows 32 bits. */
    x = (int64_t)(((uint64_t)index * 51472u) >> 30);
    x2 = (x * x) >> 15;
    x3 = (x2 * x) >> 15;
    x5 = (x3 * x2) >> 15;
    x7 = (x5 * x2) >> 15;

    value = x - (x3 / 6) + (x5 / 120) - (x7 / 5040);

    if (quadrant >= 2u) {
        value = -value;
    }

    return value;
}

/* The integer square root, rounded down, of a 64-bit unsigned value. */
static uint64_t harness_isqrt(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return result;
}

/* The gain of the last full cycles of a run: the sum of the squares of the
 * input and of the output.  The RMS ratio is used rather than the peak,
 * because a probe frequency that divides the rate exactly (16 kHz at
 * 48 kHz is three samples a cycle) has no sample on the peak; the RMS of a
 * sampled sine is its amplitude over sqrt(2) regardless.  The output is in
 * Q12 of the input's codes, so the amplitude ratio carries that shift:

 *   gain_q16^2 = 256 * out_sum / in_sum. */
static int32_t filter_gain_q16(uint32_t sample_rate, uint32_t freq)
{
    slopsix_filter filter;
    uint32_t step = (uint32_t)(((uint64_t)freq << 32) / sample_rate);
    uint32_t phase = 0;
    uint64_t in_sum = 0;
    uint64_t out_sum = 0;
    uint64_t whole;
    uint64_t rem;
    uint64_t square;
    int i;

    slopsix_filter_init(&filter, sample_rate);

    for (i = 0; i < 8192; i++) {
        /* The sine in Q15 becomes a DAC code: the full scale is 2047. */
        int32_t code = harness_sin(phase) >> 4;
        int32_t out = slopsix_filter_process(&filter, code);

        phase += step;

        if (i >= 2048) {
            in_sum += (uint64_t)((int64_t)code * code);
            out_sum += (uint64_t)((int64_t)out * out);
        }
    }

    if (in_sum == 0) {
        return 0;
    }

    /* 256 * out_sum / in_sum without overflowing the product: the quotient
     * and the fraction are taken separately. */
    whole = out_sum / in_sum;
    rem = out_sum % in_sum;
    square = whole * 256u + (rem * 256u) / in_sum;

    return (int32_t)harness_isqrt(square);
}

#ifdef SLOPSIX_DX7S
/* The DX7S tail's gain at a frequency, measured the same way but through
 * the personality's own prototype and corner. */
static int32_t filter_gain_dx7s_q16(uint32_t sample_rate, uint32_t freq)
{
    slopsix_filter filter;
    uint32_t step = (uint32_t)(((uint64_t)freq << 32) / sample_rate);
    uint32_t phase = 0;
    uint64_t in_sum = 0;
    uint64_t out_sum = 0;
    uint64_t whole;
    uint64_t rem;
    uint64_t square;
    int i;

    slopsix_dx7s_filter_init(&filter, sample_rate);

    for (i = 0; i < 8192; i++) {
        int32_t code = harness_sin(phase) >> 4;
        int32_t out = slopsix_filter_process(&filter, code);

        phase += step;

        if (i >= 2048) {
            in_sum += (uint64_t)((int64_t)code * code);
            out_sum += (uint64_t)((int64_t)out * out);
        }
    }

    if (in_sum == 0) {
        return 0;
    }

    whole = out_sum / in_sum;
    rem = out_sum % in_sum;
    square = whole * 256u + (rem * 256u) / in_sum;

    return (int32_t)harness_isqrt(square);
}
#endif

/* The closeness of a measured gain to its expectation, in percent. */
static int filter_gain_near(int32_t got, int32_t expect, int percent)
{
    int64_t delta = (int64_t)got - expect;
    int64_t tolerance = ((int64_t)expect * percent) / 100;

    if (delta < 0) {
        delta = -delta;
    }

    return delta <= tolerance;
}

static void stage_filter(void)
{
    slopsix_filter filter;
    unsigned section;
    unsigned k;

    printf("stage filter\n");

    /* The anchor rate's design is the prototype: the transform's round
     * trip is exact up to the fixed point's last bits. */
    slopsix_filter_init(&filter, SLOPSIX_DEFAULT_SAMPLE_RATE);
    for (section = 0; section < SLOPSIX_FILTER_SECTIONS; section++) {
        int match = 1;

        for (k = 0; k < 5u; k++) {
            int32_t want = filter_prototype[section][k];
            int32_t got;

            switch (k) {
            case 0: got = filter.section[section].b0; break;
            case 1: got = filter.section[section].b1; break;
            case 2: got = filter.section[section].b2; break;
            case 3: got = filter.section[section].a1; break;
            default: got = filter.section[section].a2; break;
            }

            if (got > want + 64 || got < want - 64) {
                match = 0;
            }
        }
        check(match, "the anchor rate's design is the prototype");
    }

    /* The DC gain: a constant code settles on its own value. */
    {
        int32_t value = 0;
        int i;

        slopsix_filter_init(&filter, SLOPSIX_DEFAULT_SAMPLE_RATE);
        for (i = 0; i < 4096; i++) {
            value = slopsix_filter_process(&filter, 1000);
        }
        check(value >= (1000 << SLOPSIX_FILTER_Q12) - 4 &&
              value <= (1000 << SLOPSIX_FILTER_Q12) + 4,
              "the filter's DC gain is one");

        slopsix_filter_init(&filter, SLOPSIX_DEFAULT_SAMPLE_RATE);
        for (i = 0; i < 4096; i++) {
            value = slopsix_filter_process(&filter, -1000);
        }
        check(value <= (-1000 * (1 << SLOPSIX_FILTER_Q12)) + 4 &&
              value >= (-1000 * (1 << SLOPSIX_FILTER_Q12)) - 4,
              "the filter's DC gain is one on the negative side too");
    }

    /* The response at the DX7 tail's measured frequencies: the passband,
     * the -3 dB corner at 17.02 kHz and the stopband above it. */
    check(filter_gain_near(filter_gain_q16(SLOPSIX_DEFAULT_SAMPLE_RATE, 1000),
                           65536, 2), "the passband is flat at 1 kHz");
    check(filter_gain_near(filter_gain_q16(SLOPSIX_DEFAULT_SAMPLE_RATE, 17020),
                           46400, 3), "the corner at 17.02 kHz is -3 dB");
    check(filter_gain_near(filter_gain_q16(SLOPSIX_DEFAULT_SAMPLE_RATE, 20000),
                           13170, 5), "the stopband at 20 kHz is -14 dB");

    /* The rate scaling: the corner stays where it is at another host
     * rate, and the low passband stays flat. */
    check(filter_gain_near(filter_gain_q16(48000u, 1000), 65536, 2),
          "the 48 kHz design's passband is flat");
    check(filter_gain_near(filter_gain_q16(48000u, 17020), 46400, 3),
          "the 48 kHz design keeps the corner at -3 dB");
    check(filter_gain_near(filter_gain_q16(44100u, 17020), 46400, 3),
          "the 44.1 kHz design keeps the corner at -3 dB");

    /* A host rate that cannot carry the corner: the filter is
     * transparent, because the prototype is flat below the corner and the
     * corner is at or above the host's Nyquist.  The DX7's corner is
     * 17.02 kHz, so 34.04 kHz is the boundary; the probe sits below the
     * 32 kHz design's 16 kHz Nyquist. */
    check(filter_gain_near(filter_gain_q16(32000u, 5000), 65536, 2),
          "a rate that cannot carry the corner leaves the filter flat");

#ifdef SLOPSIX_DX7S
    /* The DX7S tail's corner is 20 kHz. */
    check(filter_gain_near(filter_gain_dx7s_q16(SLOPSIX_DEFAULT_SAMPLE_RATE,
                                                1000), 65536, 2),
          "the DX7S passband is flat at 1 kHz");
    check(filter_gain_near(filter_gain_dx7s_q16(SLOPSIX_DEFAULT_SAMPLE_RATE,
                                                20000), 46400, 3),
          "the DX7S corner at 20 kHz is -3 dB");
    check(filter_gain_near(filter_gain_dx7s_q16(48000u, 20000), 46400, 3),
          "the 48 kHz DX7S design keeps the corner at -3 dB");
    check(filter_gain_near(filter_gain_dx7s_q16(44100u, 20000), 46400, 3),
          "the 44.1 kHz DX7S design keeps the corner at -3 dB");
#endif
}

static void stage_output(void)
{
    static const int32_t ladder[8] = {
        1340, 2680, 4021, 6433, 10856, 19969, 36052, 65536
    };
    const int32_t canaries[][3] = {
        /* sample, mantissa, shift - both signs of every band */
        { 0, 0, 0 },
        { 1, 1, 0 },
        { 1023, 1023, 0 },          /* 0x3FF: the last one-band value */
        { 1024, 512, 1 },           /* 0x400: the first two-band value */
        { 1025, 512, 1 },
        { 2047, 1023, 1 },
        { -1023, -1023, 0 },
        { -1024, -512, 1 },
        { 2048, 512, 2 },           /* 0x800 */
        { 4095, 1023, 2 },
        { -2048, -512, 2 },
        { 4096, 512, 3 },           /* 0x1000 */
        { 8191, 1023, 3 },
        { 8192, 1024, 3 },          /* the 14-bit boundary, no clamp yet */
        { 16383, 2047, 3 },         /* the mantissa window's top */
        { 16384, 2047, 3 },         /* clamped: floor(16384/8) is one over */
        { 32767, 2047, 3 },
        { 262016, 2047, 3 },        /* a full 16-voice bus word */
        { -4096, -512, 3 },
        { -16383, -2048, 3 },       /* the window's floor is exact */
        { -16384, -2048, 3 },
        { -16385, -2048, 3 },       /* clamped */
        { -262144, -2048, 3 }
    };
    size_t i;
    int monotonic = 1;
    int in_window = 1;

    printf("stage output\n");

    /* The compander staircase: the mantissa/shift transitions at the
     * documented thresholds, the 12-bit window's clamp beyond a single
     * voice's reach and the floor direction for negative inputs. */
    for (i = 0; i < sizeof(canaries) / sizeof(canaries[0]); i++) {
        compander_expect(canaries[i][0], canaries[i][1],
                         (unsigned)canaries[i][2], "the compander canary holds");
    }

    /* The sweep: the word's value never falls as the input rises and
     * never leaves the word's reach, and it always matches the
     * documented rule - the shift from the magnitude's thresholds, the
     * floor onto that grid, and the 12-bit window's clamp beyond it. */
    {
        int32_t previous = -40000;

        for (i = 0; i < 80000u; i++) {
            int32_t s = (int32_t)i - 40000;
            uint32_t magnitude = s < 0 ? (uint32_t)-s : (uint32_t)s;
            unsigned expect_shift = magnitude < 0x400u ? 0u
                                  : magnitude < 0x800u ? 1u
                                  : magnitude < 0x1000u ? 2u : 3u;
            int32_t expect_mantissa = slopsix_floor_div(s, expect_shift);
            slopsix_compander_word w;
            int32_t value;

            if (expect_mantissa > 2047) {
                expect_mantissa = 2047;
            } else if (expect_mantissa < -2048) {
                expect_mantissa = -2048;
            }

            slopsix_compander_quantize(s, &w);
            value = slopsix_compander_value(&w);

            if (w.shift != expect_shift || w.mantissa != expect_mantissa) {
                in_window = 0;
            }
            if (value < previous) {
                monotonic = 0;
            }
            if (value > 16376 || value < -16384) {
                in_window = 0;
            }
            previous = value;
        }
    }
    check(monotonic, "the compander's value is monotone in the input");
    check(in_window, "the compander's value is its grid floor and stays in reach");

    /* The DAC step: the 12-bit grid at one eighth of the bus unit.  It
     * does not saturate: the per-voice compander owns the 12-bit window,
     * and the bus is the analog sum of the converted voices, so a chord
     * past one voice's window must keep growing here. */
    check(slopsix_dac_step(0) == 0, "the DAC step maps zero to zero");
    check(slopsix_dac_step(7) == 0, "the DAC step floors below the grid");
    check(slopsix_dac_step(8) == 1, "the DAC step's grid is 8 bus units");
    check(slopsix_dac_step(15) == 1 && slopsix_dac_step(16) == 2,
          "the DAC step floors negative and positive alike");
    check(slopsix_dac_step(-7) == -1 && slopsix_dac_step(-8) == -1 &&
          slopsix_dac_step(-9) == -2,
          "the DAC step floors negative and positive alike");
    check(slopsix_dac_step(16376) == 2047,
          "one full-scale voice is the code window's top");
    check(slopsix_dac_step(16377) == 2047 && slopsix_dac_step(16384) == 2048,
          "a bus past one voice's window keeps growing");
    check(slopsix_dac_step(-16376) == -2047 &&
          slopsix_dac_step(-16377) == -2048 &&
          slopsix_dac_step(-16384) == -2048,
          "the negative side floors past one voice's window");
    check(slopsix_dac_step(262016) == 32752 &&
          slopsix_dac_step(-262144) == -32768,
          "a full 16-voice bus spans the wide grid, not one voice's window");
    {
        int monotone = 1;

        for (i = 0; i < 80000u; i++) {
            int32_t bus = (int32_t)i - 40000;
            int32_t code = slopsix_dac_step(bus);
            int32_t lower = slopsix_dac_step(bus - 1);

            if (code < lower ||
                code != slopsix_floor_div(bus, SLOPSIX_DAC_GRID_SHIFT)) {
                monotone = 0;
            }
        }
        check(monotone == 1, "the DAC step is monotone on the wide grid");
    }

#if SLOPSIX_HAS_DX7S
    /* The DX7S's PCM54HP output is 16-bit linear: its DAC step is a plain
     * 16-bit grid with rail saturation, not the Mk1's 12-bit grid, and
     * the per-voice bus adds the raw operator words without the
     * compander's low-bit truncation.  The grid sits at half a bus unit,
     * so the 16-bit DAC spans the same analog window as the Mk1's
     * 12-bit one and the two personalities share one reference level. */
    check(slopsix_dx7s_dac_step(0) == 0,
          "the DX7S DAC step maps zero to zero");
    check(slopsix_dx7s_dac_step(1) == 2 &&
          slopsix_dx7s_dac_step(0x1234) == 0x2468,
          "the DX7S DAC step keeps all 16 bits, at half a bus unit");
    check(slopsix_dx7s_dac_step(-1) == -2 &&
          slopsix_dx7s_dac_step(-0x1234) == -0x2468,
          "the DX7S DAC step keeps negative low bits too");
    check(slopsix_dx7s_dac_step(16383) == 32766 &&
          slopsix_dx7s_dac_step(-16384) == -32768,
          "the DX7S DAC step passes the 16-bit rails");
    check(slopsix_dx7s_dac_step(16384) == 32767 &&
          slopsix_dx7s_dac_step(-16385) == -32768,
          "the DX7S DAC step saturates past the rails");
    check(slopsix_dx7s_dac_step(16376) == 32752,
          "one full-scale voice lands on the DX7S DAC's top code");
    {
        int monotone = 1;

        for (i = 0; i < 40000u; i++) {
            int32_t bus = (int32_t)i - 20000;
            int32_t code = slopsix_dx7s_dac_step(bus);
            int32_t lower = slopsix_dx7s_dac_step(bus - 1);

            if (code < lower) {
                monotone = 0;
            }
            if (code > 32767 || code < -32768) {
                monotone = 0;
            }
        }
        check(monotone == 1,
              "the DX7S DAC step is monotone and stays in the window");
    }
    {
        const int32_t raw[4] = { 0x1234, -0x4321, 1, -1 };
        int32_t bus[4] = { 1, 2, 3, 4 };
        int i2;

        slopsix_dx7s_voice_bus(raw, bus, 4u);
        for (i2 = 0; i2 < 4; i2++) {
            int32_t expect = (int32_t)(i2 + 1) + raw[i2];

            if (bus[i2] != expect) {
                break;
            }
        }
        check(i2 == 4,
              "the DX7S voice bus adds raw voice words, low bits intact");
    }
    check(slopsix_dx7s_output_scale(1 << SLOPSIX_FILTER_Q12,
            SLOPSIX_Q16_ONE) == 1,
          "the DX7S conversion keeps a one-LSB 16-bit code");
    check(slopsix_dx7s_output_scale(32767 << SLOPSIX_FILTER_Q12,
            SLOPSIX_Q16_ONE) == 16384,
          "the DX7S conversion keeps the default headroom below int16");
#endif

    /* The master ladder: the resistor string's taps as exact Q16 gains. */
    for (i = 0; i < 8u; i++) {
        if (slopsix_master_gain_q16((unsigned)i) != ladder[i]) {
            break;
        }
    }
    check(i == 8u, "the ladder's eight taps match the resistor string");
    check(slopsix_master_gain_q16(8u) == ladder[7],
          "a level past the top of the ladder clamps to it");

    /* The final conversion: rounding to the int16 grid at unity gain.
     * The value is the analog stage's output in Q12 of a DAC code, so
     * the code window's rails are the Q12 values of 2047 and -2048.
     * These expectations are the default reference
     * (SLOPSIX_OUTPUT_HEADROOM_BITS = 1); removing the headroom doubles
     * every value here. */
    check(slopsix_output_scale(2047 << SLOPSIX_FILTER_Q12,
                               SLOPSIX_Q16_ONE) == 16376,
          "the code window's top lands on 16376 at unity gain");
    check(slopsix_output_scale(-2048 * (1 << SLOPSIX_FILTER_Q12),
                               SLOPSIX_Q16_ONE) == -16384,
          "the code window's floor lands on -16384");
    check(slopsix_output_scale(2046 << SLOPSIX_FILTER_Q12,
                               SLOPSIX_Q16_ONE) == 16368,
          "unity gain keeps the int16 output on the 12-bit grid");
    check(slopsix_output_scale(100 << SLOPSIX_FILTER_Q12,
                               SLOPSIX_Q16_ONE) == 800,
          "unity gain keeps the int16 output on the 12-bit grid");
    check(slopsix_output_scale(2047 << SLOPSIX_FILTER_Q12, 36052) == 9009,
          "a ladder tap scales the code");
    check(slopsix_output_scale(-2048 * (1 << SLOPSIX_FILTER_Q12), 1340) == -335,
          "the quietest tap scales the code");
    check(slopsix_output_scale(2047 << SLOPSIX_FILTER_Q12,
                               SLOPSIX_Q16_ONE) == 16376 &&
          slopsix_output_scale(4096 << SLOPSIX_FILTER_Q12,
                               SLOPSIX_Q16_ONE) == 32767,
          "the conversion saturates at the int16 rails");

    {
        slopsix_context ctx;
        slopsix_context_state *state;
        int16_t l1, r1;

        ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
        if (ctx == NULL) {
            check(0, "create_context returns a context");
            return;
        }
        state = SLOPSIX_CONTEXT(ctx);

        /* The default context: the hardware's reset volume. */
        check(state->master_volume == 7u,
              "the default master volume is the hardware's reset value");

        /* The composite at default settings: full scale in, one voice's
         * window top out, left and right identical.  The analog tail's
         * filter is stateful, so the constant bus is settled first. */
        output_settle(state, 16376, &l1, &r1);
        check(output_near(l1, 16376, 2) && output_near(r1, 16376, 2),
              "the mixed bus's full scale lands on the code window's top");
        output_settle(state, 16384, &l1, &r1);
        check(output_near(l1, 16384, 2) && output_near(r1, 16384, 2),
              "a bus past one voice's window keeps growing");
        output_settle(state, 32752, &l1, &r1);
        check(output_near(l1, 32752, 2) && output_near(r1, 32752, 2),
              "a two-voice bus stays below the int16 rails");
        output_settle(state, 40000, &l1, &r1);
        check(output_near(l1, 32767, 2) && output_near(r1, 32767, 2),
              "only the int16 conversion saturates a loud polyphonic bus");
        output_settle(state, -16384, &l1, &r1);
        check(output_near(l1, -16384, 2) && output_near(r1, -16384, 2),
              "the negative side keeps growing past one voice's window");

        /* Determinism: the same state and the same input sequence produce
         * the same output.  The filter is stateful, so the replay starts
         * from a reset context. */
        {
            int16_t first[8];
            int16_t second[8];
            int same = 1;
            int i;

            slopsix_reset(ctx);
            for (i = 0; i < 4; i++) {
                slopsix_output_mix(state, 12345, &first[i * 2],
                                   &first[i * 2 + 1]);
            }
            slopsix_reset(ctx);
            for (i = 0; i < 4; i++) {
                slopsix_output_mix(state, 12345, &second[i * 2],
                                   &second[i * 2 + 1]);
            }
            for (i = 0; i < 8; i++) {
                if (first[i] != second[i]) {
                    same = 0;
                }
            }
            check(same, "the mix is deterministic");
        }

        /* The volume ladder through the composite. */
        slopsix_set_midi_volume(ctx, 0u);
        output_settle(state, 16376, &l1, &r1);
        check(output_near(l1, 335, 2) && output_near(r1, 335, 2),
              "volume 0 is the ladder's bottom tap, not silence");
        slopsix_set_midi_volume(ctx, 3u);
        output_settle(state, 16376, &l1, &r1);
        check(output_near(l1, 1607, 2) && output_near(r1, 1607, 2),
              "volume 3's tap scales the mixed code");
        slopsix_set_midi_volume(ctx, 7u);

        /* The host output volume: a plain Q16 gain folded into the
         * conversion's cached gain at setter time.  At the default ladder
         * (unity) half volume halves the converted code. */
        check(state->output_volume_q16 == SLOPSIX_Q16_ONE,
              "the default output volume is unity");
        slopsix_set_output_gain(ctx, SLOPSIX_Q16_ONE / 2);
        output_settle(state, 16376, &l1, &r1);
        check(output_near(l1, 8188, 2) && output_near(r1, 8188, 2),
              "half output volume halves the converted code");
        slopsix_set_output_gain(ctx, 0);
        output_settle(state, 16376, &l1, &r1);
        check(l1 == 0 && r1 == 0, "zero output volume is silence");
        slopsix_set_output_gain(ctx, SLOPSIX_Q16_ONE);

        /* The composition of the two gains: the ladder tap times the host's
         * output volume, in whichever order the two are set. */
        slopsix_set_midi_volume(ctx, 3u);
        slopsix_set_output_gain(ctx, SLOPSIX_Q16_ONE / 2);
        check(state->master_gain_q16 == (6433 * (SLOPSIX_Q16_ONE / 2)) >> 16,
              "the gain composes the ladder tap with the output volume");
        output_settle(state, 16376, &l1, &r1);
        check(output_near(l1, 804, 2) && output_near(r1, 804, 2),
              "the composed gain scales the converted code");
        slopsix_set_midi_volume(ctx, 7u);
        check(state->master_gain_q16 == (SLOPSIX_Q16_ONE / 2),
              "changing the master volume keeps the host's output volume");
        slopsix_set_output_gain(ctx, SLOPSIX_Q16_ONE);
        slopsix_set_midi_volume(ctx, 7u);

#if SLOPSIX_HAS_EXTRAS
        /* The pan law through the composite: centre is bit-identical
         * mono-in-both-channels, the extremes are hard splits.  The bus
         * 8000 maps to code 1000, which at unity gain is 8000. */
        output_settle(state, -12800, &l1, &r1);
        check(output_near(l1, -12800, 2) && output_near(r1, -12800, 2),
              "the default centre keeps both channels identical");
        slopsix_set_pan(ctx, 0);
        output_settle(state, 8000, &l1, &r1);
        check(output_near(l1, 8000, 2) && r1 == 0,
              "hard left carries the mono signal alone");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE);
        output_settle(state, 8000, &l1, &r1);
        check(l1 == 0 && output_near(r1, 8000, 2),
              "hard right carries the mono signal alone");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE / 4);
        output_settle(state, 8000, &l1, &r1);
        check(output_near(l1, 8000, 2) && output_near(r1, 4000, 2),
              "the left half keeps the mono level, the right fades");
        slopsix_set_pan(ctx, 3 * SLOPSIX_Q16_ONE / 4);
        output_settle(state, 8000, &l1, &r1);
        check(output_near(l1, 4000, 2) && output_near(r1, 8000, 2),
              "the right half keeps the mono level, the left fades");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE / 2);
        output_settle(state, -12800, &l1, &r1);
        check(output_near(l1, -12800, 2) && output_near(r1, -12800, 2),
              "centre is bit-identical, negative codes included");

        /* The pan law's own clamps, called directly on a code. */
        {
            int32_t l, r;

            slopsix_pan_split(SLOPSIX_Q16_ONE + 5, 100, &l, &r);
            check(l == 0 && r == 100, "the pan law clamps past hard right");
            slopsix_pan_split(-5, 100, &l, &r);
            check(l == 100 && r == 0, "the pan law clamps past hard left");
            slopsix_pan_split(SLOPSIX_Q16_ONE / 2, -12345, &l, &r);
            check(l == -12345 && r == -12345,
                  "the pan law's centre is exact for negative values");
            slopsix_pan_split(SLOPSIX_Q16_ONE / 4, -12345, &l, &r);
            check(l == -12345 && r == -6173,
                  "the pan law's Q15 amounts floor the negative half");
        }
#else
        /* Without the extras the mono code lands in both channels
         * whatever a host stores in the state. */
        output_settle(state, 8000, &l1, &r1);
        check(output_near(l1, 8000, 2) && output_near(r1, 8000, 2),
              "without the extras both channels carry the mono signal");
#endif

        slopsix_destroy_context(ctx);
    }
}

/* --------------------------------------------------- Phase 5: the render
 *
 * The live voice path's checks: a parsed patch, a keyed note, the pitch
 * it renders at, the level it holds, the release it falls into, the
 * determinism of the whole chain, and a sixteen-voice soak.  The stage
 * writes its takes into build/ as WAV files when it can, so a session
 * can listen to them.
 *
 * The reference patch is the engine's own take on a minimal voice:
 * algorithm 1, all operators ratio 1, only operator 1 (dump block 5)
 * sounding, fast envelopes, L4 = 0 for a clean release.  With OSC KEY
 * SYNC on the phases reset at key-on, so the take is deterministic from
 * the phase up. */

#define RENDER_FRAMES SLOPSIX_DEFAULT_SAMPLE_RATE

/* The reference patch's name field width (the dump's name is ten bytes;
 * the helper writes the first eight and leaves the padding zeroed). */
#define RENDER_NAME_LEN 8

static int16_t render_buf_a[RENDER_FRAMES * 2];
static int16_t render_buf_b[RENDER_FRAMES * 2];

static void write_wav(const char *path, const int16_t *stereo, size_t frames)
{
    uint8_t header[44];
    uint32_t data_len = (uint32_t)(frames * 2u * 2u);
    uint32_t riff_len = data_len + 36u;
    uint32_t rate = SLOPSIX_DEFAULT_SAMPLE_RATE;
    uint32_t byte_rate = rate * 4u;
    FILE *file;

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)riff_len; header[5] = (uint8_t)(riff_len >> 8);
    header[6] = (uint8_t)(riff_len >> 16); header[7] = (uint8_t)(riff_len >> 24);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16u; header[17] = 0u; header[18] = 0u; header[19] = 0u;
    header[20] = 1u; header[21] = 0u;          /* PCM */
    header[22] = 2u; header[23] = 0u;          /* stereo */
    header[24] = (uint8_t)rate; header[25] = (uint8_t)(rate >> 8);
    header[26] = (uint8_t)(rate >> 16); header[27] = (uint8_t)(rate >> 24);
    header[28] = (uint8_t)byte_rate; header[29] = (uint8_t)(byte_rate >> 8);
    header[30] = (uint8_t)(byte_rate >> 16); header[31] = (uint8_t)(byte_rate >> 24);
    header[32] = 4u; header[33] = 0u;          /* block align */
    header[34] = 16u; header[35] = 0u;         /* bits per sample */
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)data_len; header[41] = (uint8_t)(data_len >> 8);
    header[42] = (uint8_t)(data_len >> 16); header[43] = (uint8_t)(data_len >> 24);

    file = fopen(path, "wb");
    if (file == NULL) {
        return;
    }
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(stereo, 2, frames * 2u, file) != frames * 2u) {
        /* A check run without a writable build/ still checks. */
    }
    fclose(file);
}

/* The mean square of the left channel over one window. */
static int64_t render_ms(const int16_t *stereo, size_t start, size_t len)
{
    int64_t sum = 0;
    size_t i;

    for (i = start; i < start + len; i++) {
        int32_t s = stereo[i * 2u];

        sum += (int64_t)s * s;
    }
    return sum / (int64_t)len;
}

static int32_t render_peak(const int16_t *stereo, size_t len)
{
    int32_t peak = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        int32_t s = stereo[i * 2u];

        if (s < 0) {
            s = -s;
        }
        if (s > peak) {
            peak = s;
        }
    }
    return peak;
}

static int render_zerocross(const int16_t *stereo, size_t start, size_t len)
{
    int count = 0;
    size_t i;

    for (i = start + 1u; i < start + len; i++) {
        if ((stereo[(i - 1u) * 2u] < 0) != (stereo[i * 2u] < 0)) {
            count++;
        }
    }
    return count;
}

static void make_init_patch(uint8_t *dump)
{
    static const char name[RENDER_NAME_LEN + 1u] = "RENDRTST";
    unsigned block;

    memset(dump, 0, SLOPSIX_VOICE_DUMP_SIZE);

    for (block = 0; block < 6u; block++) {
        uint8_t *op = dump + block * SLOPSIX_DUMP_OP_BLOCK;

        op[0] = 99u; op[1] = 99u; op[2] = 99u; op[3] = 99u;   /* rates */
        op[4] = 99u; op[5] = 99u; op[6] = 99u; op[7] = 0u;    /* levels */
        /* Block 5 is operator 1, the algorithm's own output operator:
         * only it sounds. */
        op[16] = (block == 5u) ? 99u : 0u;
        op[18] = 1u;                                          /* coarse */
        op[20] = 7u;                                          /* detune */
    }

    dump[126] = 99u; dump[127] = 99u; dump[128] = 99u; dump[129] = 99u;
    dump[130] = 50u; dump[131] = 50u; dump[132] = 50u; dump[133] = 50u;
    dump[134] = 0u;                       /* algorithm 1 (0-based field) */
    dump[136] = 1u;                       /* OSC KEY SYNC on */
    dump[144] = 24u;                      /* transpose neutral */
    memcpy(dump + SLOPSIX_DUMP_TRANSPOSE + 1u, name, RENDER_NAME_LEN);
}

static void stage_render(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];
    uint8_t dump2[SLOPSIX_VOICE_DUMP_SIZE];
    size_t i;
    int64_t ms_first = 0;
    int32_t peak;
    int steady = 1;
    int crossings;

    printf("stage render\n");

    /* The dump round-trips before it is parsed, so the checked take is
     * the parser's own reading. */
    make_init_patch(dump);
    check(memcmp(dump + SLOPSIX_DUMP_TRANSPOSE + 1u, "RENDRTST", 8) == 0,
          "the reference patch's name sits in the name field");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK &&
          slopsix_get_voice(ctx, dump2, sizeof(dump2)) == SLOPSIX_OK &&
          memcmp(dump, dump2, sizeof(dump)) == 0,
          "the voice dump round-trips");
    check(state->patch.algorithm == 0u && state->patch.feedback == 0u &&
          state->patch.transpose == 0,
          "the parser reads the reference patch's globals");

    /* One full second of A4: the note keys, the envelope attack, the
     * sustain, then the release. */
    memset(render_buf_a, 0x77, sizeof(render_buf_a));
    slopsix_note_on(ctx, 69, 100);
    slopsix_process(ctx, render_buf_a, RENDER_FRAMES);

    /* One full-scale-ish carrier: the velocity route's own fixed
     * attenuation and the algorithm's carrier-count compensation sit
     * between the patch's level 99 and the compander's word, so the
     * peak lands at half the single-voice window's top. */
    peak = render_peak(render_buf_a, RENDER_FRAMES);
    check(peak >= 3500 && peak <= 16376,
          "one voice's peak sits in the compander's window");
    check(render_ms(render_buf_a, 9000, 7000) > (int64_t)2 * 1000 * 1000,
          "the sustained note renders an audible level");

    /* The sustain is steady: five windows agree within three percent. */
    ms_first = render_ms(render_buf_a, 9000, 7000);
    for (i = 1; i < 5u; i++) {
        int64_t ms = render_ms(render_buf_a, 9000u + 7000u * i, 7000);
        int64_t hi = ms_first * 103;
        int64_t lo = ms_first * 97;

        if (ms * 100 < lo || ms * 100 > hi) {
            steady = 0;
        }
    }
    check(steady, "the sustain holds one level");
    crossings = render_zerocross(render_buf_a, 9800, 39200);
    check(crossings >= 686 && crossings <= 722,
          "the note renders at 440 Hz (the zero crossings)");

    {
        int64_t sum = 0;

        for (i = 9800u; i < 49000u; i++) {
            sum += render_buf_a[i * 2u];
        }
        sum /= (int64_t)(49000u - 9800u);
        if (sum < 0) {
            sum = -sum;
        }
        check(sum < 100, "the rendered note carries no DC");
    }

    {
        int both = 1;

        for (i = 0; i < RENDER_FRAMES; i++) {
            if (render_buf_a[i * 2u] != render_buf_a[i * 2u + 1u]) {
                both = 0;
            }
        }
        check(both, "the mono output lands in both channels");
    }

    write_wav("build/render_sine.wav", render_buf_a, RENDER_FRAMES);

    /* The release: a half second after key-off the voice is silent. */
    slopsix_note_off(ctx, 69);
    memset(render_buf_b, 0x77, sizeof(render_buf_b));
    slopsix_process(ctx, render_buf_b, RENDER_FRAMES / 2);
    check(render_ms(render_buf_b, RENDER_FRAMES / 2 - 1000, 1000) == 0,
          "the release ends in silence");
    write_wav("build/render_release.wav", render_buf_b, RENDER_FRAMES / 2);

    /* A patch load while a note is held silences it: the hardware's patch
     * activation resets every EGS voice before loading the new registers.
     * The held key keeps its voice allocated, as the firmware's key-event
     * bookkeeping does. */
    slopsix_reset(ctx);
    check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK,
          "the patch loads before the held note");
    slopsix_note_on(ctx, 69, 100);
    slopsix_process(ctx, render_buf_b, RENDER_FRAMES / 2);
    check(render_ms(render_buf_b, RENDER_FRAMES / 2 - 1000, 1000) > 0,
          "the held note sounds before the patch load");
    check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK,
          "a patch load while the note is held succeeds");
    check(state->engines[0].eg[0].level ==
              (SLOPSIX_EG_MIN_Q8 << SLOPSIX_EG_FRAC),
          "the patch load silences the held voice's envelope");
    check(state->voices[0].state == SLOPSIX_VOICE_ON &&
          state->voices[0].held,
          "the held key keeps its voice allocated");
    memset(render_buf_b, 0x77, sizeof(render_buf_b));
    slopsix_process(ctx, render_buf_b, RENDER_FRAMES / 2);
    check(render_ms(render_buf_b, RENDER_FRAMES / 2 - 1000, 1000) == 0,
          "a patch load silences a sounding voice");
    slopsix_note_off(ctx, 69);
    slopsix_process(ctx, render_buf_b, RENDER_FRAMES / 4);
    check(render_ms(render_buf_b, RENDER_FRAMES / 4 - 1000, 1000) == 0,
          "the silenced voice stays silent through its key release");

    /* Determinism across a reset: the same calls render the same take. */
    slopsix_reset(ctx);
    check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK,
          "the reset context takes the patch");
    slopsix_note_on(ctx, 69, 100);
    slopsix_process(ctx, render_buf_b, RENDER_FRAMES);
    check(memcmp(render_buf_a, render_buf_b, sizeof(render_buf_a)) == 0,
          "the reset context renders the identical take");

    /* The sixteen-voice soak: sixteen notes of one patch, then a
     * seventeenth that steals the oldest; the whole take is
     * deterministic across a reset. */
    slopsix_reset(ctx);
    slopsix_set_voice(ctx, dump, sizeof(dump));
    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        slopsix_note_on(ctx, (uint8_t)(48u + i), 100);
    }
    check(state->voices[SLOPSIX_VOICE_COUNT - 1u].note == 63u &&
          state->voices[SLOPSIX_VOICE_COUNT - 1u].state ==
              SLOPSIX_VOICE_ON,
          "the sixteen voices hold sixteen notes");

    slopsix_process(ctx, render_buf_a, 8000);
    peak = render_peak(render_buf_a, 8000);
    check(peak <= 32768, "the chord saturates inside the rails, not past");
    write_wav("build/render_chord.wav", render_buf_a, 8000);

    slopsix_note_on(ctx, 64, 100);
    check(state->voices[0].note == 64u &&
          state->voices[0].state == SLOPSIX_VOICE_ON,
          "the chord's seventeenth note steals the oldest voice");

    slopsix_reset(ctx);
    slopsix_set_voice(ctx, dump, sizeof(dump));
    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        slopsix_note_on(ctx, (uint8_t)(48u + i), 100);
    }
    slopsix_process(ctx, render_buf_b, 8000);
    check(memcmp(render_buf_a, render_buf_b, 8000 * 2u * 2u) == 0,
          "the chord render is deterministic across a reset");

    slopsix_destroy_context(ctx);
}

/* ------------------------------------------------ the block render path */

/* The state fields the render paths mutate (the block renderer and the
 * walk must agree on all of them). */
static void block_probe(const slopsix_voice_engine *e, uint32_t *phases,
                        int32_t *eg_level, int32_t *regs, uint8_t *eg_stage)
{
    unsigned k;

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        phases[k] = e->ops[k].phase;
        eg_level[k] = e->eg[k].level;
        eg_stage[k] = e->eg[k].stage;
        /* the EG's block-cadence fields move even when the cadence is
         * off, so they ride along in the same words */
        eg_level[k] ^= (int32_t)e->eg[k].block_pos;
        eg_level[k] ^= e->eg[k].block_level << 3;
        eg_level[k] ^= e->eg[k].block_step << 5;
    }
    regs[0] = e->modout;
    regs[1] = e->mren;
    regs[2] = e->fren1;
    regs[3] = e->fren2;
}

static int block_probe_equal(const slopsix_voice_engine *a,
                             const slopsix_voice_engine *b)
{
    uint32_t phases[2][SLOPSIX_OP_COUNT];
    int32_t levels[2][SLOPSIX_OP_COUNT];
    int32_t regs[2][4];
    uint8_t stages[2][SLOPSIX_OP_COUNT];

    block_probe(a, phases[0], levels[0], regs[0], stages[0]);
    block_probe(b, phases[1], levels[1], regs[1], stages[1]);

    return memcmp(phases[0], phases[1], sizeof(phases[0])) == 0 &&
           memcmp(levels[0], levels[1], sizeof(levels[0])) == 0 &&
           memcmp(regs[0], regs[1], sizeof(regs[0])) == 0 &&
           memcmp(stages[0], stages[1], sizeof(stages[0])) == 0;
}

static uint32_t block_seed;

static uint32_t block_rnd(void)
{
    block_seed = block_seed * 1103515245u + 12345u;
    return (block_seed >> 8) & 0xFFFFFF;
}

/* A randomized voice engine: the envelope generators live, the carry-in
 * registers and phases arbitrary, the amplitude modulation live. */
static void block_random_engine(slopsix_voice_engine *e, int eg_block_mode)
{
    unsigned k;
    uint8_t rates[4];
    uint8_t levels[4];

    memset(e, 0, sizeof(*e));

    for (k = 0; k < SLOPSIX_OP_COUNT; k++) {
        unsigned i;

        for (i = 0; i < 4u; i++) {
            rates[i] = (uint8_t)(block_rnd() % 100u);
            levels[i] = (uint8_t)(block_rnd() % 100u);
        }
        e->params[k].log_amp_q10 = (int32_t)(block_rnd() % 5000u) << 2;
        e->params[k].ams_sens = (uint8_t)(block_rnd() % 4u);
        e->params[k].ams_reduction_q8 =
            slopsix_ams_reduction_q8(e->params[k].ams_sens,
                                     (uint8_t)(block_rnd() % 256u));
        e->ops[k].phase = block_rnd();
        e->ops[k].increment = 1000u + block_rnd() % 400000u;
        slopsix_eg_init(&e->eg[k], rates, levels,
                        (int32_t)(block_rnd() % 4000u),
                        (uint8_t)(block_rnd() % 32u),
                        SLOPSIX_DEFAULT_SAMPLE_RATE);
        if (eg_block_mode) {
            e->eg[k].block_mode = 1u;
        }
    }

    e->feedback_level = (uint8_t)(block_rnd() % 8u);
    e->modout = (int32_t)(block_rnd() % 65536u) - 32768;
    e->mren = (int32_t)(block_rnd() % 65536u) - 32768;
    e->fren1 = (int32_t)(block_rnd() % 65536u) - 32768;
    e->fren2 = (int32_t)(block_rnd() % 65536u) - 32768;
}

/* One algorithm's voice-level check: the walk and the block renderer
 * must agree sample for sample and leave the same engine state, for
 * every span shape the render call's sub-blocks can produce - including
 * spans past the block size and single-sample calls. */
static void block_check_algorithm(unsigned algorithm, int eg_block_mode)
{
    static const unsigned shapes[6][6] = {
        { 64, 64, 64, 64, 108, 0 },  /* 300 = block-aligned + a tail */
        { 1, 1, 1, 1, 296, 0 },      /* single-sample calls */
        { 7, 64, 1, 128, 100, 0 },   /* odd lengths, one past a block */
        { 299, 1, 0, 0, 0, 0 },      /* a tail of one */
        { 300, 0, 0, 0, 0, 0 },      /* one call past the block size */
        { 63, 64, 173, 0, 0, 0 },    /* misaligned every which way */
    };
    const slopsix_algorithm *alg = slopsix_algorithm_get(algorithm);
    unsigned shape;
    unsigned caseid;

    for (caseid = 0; caseid < 2u; caseid++) {
        for (shape = 0; shape < 6u; shape++) {
            slopsix_voice_engine ref;
            slopsix_voice_engine blk;
            struct {
                int32_t ref[300];
                int32_t blk[300];
            } out;
            unsigned spans[6];
            unsigned pos = 0;
            unsigned s;

            for (s = 0; s < 6u; s++) {
                spans[s] = shapes[shape][s];
            }

            block_seed = 1000u + algorithm * 97u + shape * 7u +
                         (uint32_t)eg_block_mode * 131u;
            block_random_engine(&ref, eg_block_mode);
            blk = ref;

            for (s = 0; s < 6u; s++) {
                slopsix_block_scratch scratch;
                unsigned count = spans[s];
                unsigned t;

                if (count == 0u) {
                    break;
                }
                if (pos + count > 300u) {
                    count = 300u - pos;
                }

                memset(&scratch, 0, sizeof(scratch));
                for (t = 0; t < count; t++) {
                    slopsix_voice_render_sample(&ref, alg,
                                                &out.ref[pos + t]);
                }
                slopsix_voice_render_block(&blk, alg, &scratch,
                                           &out.blk[pos], count);
                pos += count;

                if (!block_probe_equal(&ref, &blk)) {
                    check(0, "the block render leaves the walk's engine "
                             "state after each span");
                    return;
                }
            }

            for (s = 0; s < pos; s++) {
                if (out.ref[s] != out.blk[s]) {
                    (void)out;
                    check(out.ref[s] == out.blk[s],
                          "the block render matches the walk sample for "
                          "sample");
                    return;
                }
            }
        }
    }
}

/* ------------------------------------------- Phase D7S-2: per-voice LFO */

#if SLOPSIX_HAS_DX7S
/* Pre-output parity capture: the DX7 and DX7S deliberately end in
 * different output stages (the DX7S's 16-bit DAC tail), so a neutral
 * DX7S must equal the DX7 at the mixed bus, before the output stage.
 * Swapping each context's output_mix for this recorder compares exactly
 * that. */
#define PARITY_BUS_MAX 512
static int32_t parity_bus_a[PARITY_BUS_MAX];
static int32_t parity_bus_b[PARITY_BUS_MAX];
static int32_t *parity_dst;
static size_t parity_len;

static void parity_capture_mix(void *vstate, int32_t bus, int16_t *left,
    int16_t *right)
{
    (void)vstate;
    (void)left;
    (void)right;
    if (parity_len < PARITY_BUS_MAX) {
        parity_dst[parity_len++] = bus;
    }
}
#endif

static void stage_personality_params(void)
{
    slopsix_context ctx;

    printf("stage personality params\n");

    /* The DX7 personality has none of the DX7S-only parameters: every
     * setter must say so and leave the context alone, so a host that loads
     * a DX7S patch and applies its extension parameters through one shared
     * path cannot make the DX7 sound like a DX7S. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7 context");
    if (ctx != NULL) {
        check(slopsix_set_key_mode(ctx, SLOPSIX_KEY_MONO) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no key mode");
        check(slopsix_set_unison_detune(ctx, 7u) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no unison detune");
        check(slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_HALF_OCTAVE) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no pitch-EG range");
        check(slopsix_set_peg_velocity(ctx, true) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no pitch-EG velocity switch");
        check(slopsix_set_peg_rate_scaling(ctx, 7u) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no pitch-EG rate scaling");

        /* A malformed value is reported as such regardless of engine. */
        check(slopsix_set_key_mode(ctx, (slopsix_key_mode)9) ==
              SLOPSIX_ERR_INVALID,
              "an unknown key mode is rejected");
        check(slopsix_set_peg_range(ctx, (slopsix_peg_range)9) ==
              SLOPSIX_ERR_INVALID,
              "an unknown pitch-EG range is rejected");
        slopsix_destroy_context(ctx);
    }

#if SLOPSIX_HAS_DX7S
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7S context");
    if (ctx != NULL) {
        slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);

        check(state->key_mode == (uint8_t)SLOPSIX_KEY_POLY &&
              state->unison_detune == 0u &&
              state->patch.peg_range == SLOPSIX_PEG_RANGE_DEFAULT &&
              state->patch.peg_velocity == 0u &&
              state->patch.peg_rate_scaling == 0u,
              "the DX7S-only parameters default to the Mk1-equivalent "
              "behaviour");

        check(slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_MONO) ==
              SLOPSIX_OK &&
              state->key_mode == (uint8_t)SLOPSIX_KEY_UNISON_MONO,
              "the DX7S takes a key mode");
        check(slopsix_set_unison_detune(ctx, 5u) == SLOPSIX_OK &&
              state->unison_detune == 5u,
              "the DX7S takes a unison detune");
        check(slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_HALF_OCTAVE) ==
              SLOPSIX_OK &&
              state->patch.peg_range ==
                  (uint8_t)SLOPSIX_PEG_RANGE_HALF_OCTAVE,
              "the DX7S takes a pitch-EG range");
        check(slopsix_set_peg_velocity(ctx, true) == SLOPSIX_OK &&
              state->patch.peg_velocity == 1u,
              "the DX7S takes the pitch-EG velocity switch");
        check(slopsix_set_peg_rate_scaling(ctx, 6u) == SLOPSIX_OK &&
              state->patch.peg_rate_scaling == 6u,
              "the DX7S takes a pitch-EG rate scaling");

        check(slopsix_set_unison_detune(ctx, 9u) == SLOPSIX_ERR_RANGE &&
              state->unison_detune == 7u,
              "an oversized unison detune clamps to 7");
        check(slopsix_set_peg_rate_scaling(ctx, 9u) == SLOPSIX_ERR_RANGE &&
              state->patch.peg_rate_scaling == 7u,
              "an oversized pitch-EG rate scaling clamps to 7");
        check(slopsix_set_key_mode(ctx, (slopsix_key_mode)9) ==
              SLOPSIX_ERR_INVALID &&
              state->key_mode == (uint8_t)SLOPSIX_KEY_UNISON_MONO,
              "a rejected key mode leaves the mode unchanged");
        check(slopsix_set_peg_range(ctx, (slopsix_peg_range)9) ==
              SLOPSIX_ERR_INVALID &&
              state->patch.peg_range ==
                  (uint8_t)SLOPSIX_PEG_RANGE_HALF_OCTAVE,
              "a rejected pitch-EG range leaves the range unchanged");

        slopsix_reset(ctx);
        check(state->key_mode == (uint8_t)SLOPSIX_KEY_POLY &&
              state->unison_detune == 0u &&
              state->patch.peg_range == SLOPSIX_PEG_RANGE_DEFAULT &&
              state->patch.peg_velocity == 0u &&
              state->patch.peg_rate_scaling == 0u,
              "reset restores the DX7S-only parameters' defaults");
        slopsix_destroy_context(ctx);
    }
#endif
}

static void stage_plfo(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
#if SLOPSIX_HAS_DX7S
    unsigned v;
    unsigned i;
#endif

    printf("stage plfo\n");

    /* The Mk1 path: there is no per-voice LFO mode, and every voice reads
     * the one global amp-mod register. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7 context");
    if (ctx != NULL) {
        state = SLOPSIX_CONTEXT(ctx);
        check(slopsix_set_lfo_mode(ctx, SLOPSIX_LFO_SINGLE) ==
              SLOPSIX_ERR_UNSUPPORTED,
              "the DX7 has no per-voice LFO mode");
        state->lfo.amp_mod_register = 42u;
        state->engines[0].params[0].ams_sens = 3u;
        state->engines[1].params[0].ams_sens = 3u;
        slopsix_voice_engine_update_ams(state, &state->engines[0]);
        slopsix_voice_engine_update_ams(state, &state->engines[1]);
        check(state->engines[0].params[0].ams_reduction_q8 ==
              state->engines[1].params[0].ams_reduction_q8,
              "the Mk1 voices share the global amp-mod register");
        slopsix_destroy_context(ctx);
    }

#if SLOPSIX_HAS_DX7S
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7S context");
    if (ctx == NULL) {
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    check(state->lfo_mode == (uint8_t)SLOPSIX_LFO_SINGLE,
          "the DX7S LFO defaults to Single");
    check(slopsix_set_lfo_mode(ctx, SLOPSIX_LFO_MULTI) == SLOPSIX_OK &&
          state->lfo_mode == (uint8_t)SLOPSIX_LFO_MULTI,
          "the DX7S LFO mode switches to Multi");
    check(slopsix_set_lfo_mode(ctx, (slopsix_lfo_mode)9) ==
          SLOPSIX_ERR_INVALID,
          "an unknown LFO mode is rejected");

    /* The per-voice engine exercises the shared wave recipes: every voice
     * at a different phase and waveform must compute what the shared
     * phase-to-amplitude step computes. */
    for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
        slopsix_lfo *lfo = &state->engines[v].lfo;

        lfo->phase = (uint16_t)(0x0500u * v + 0x1234u);
        lfo->phase_increment = 0u;
        lfo->delay_increment = 0u;
        lfo->delay_acc = 0u;
        lfo->fade = 0u;
        lfo->sh_acc = 0u;
        lfo->amplitude = 0;
        lfo->waveform = (uint8_t)(v % 6u);
        lfo->pitch_mod_depth = 0u;
        lfo->amp_mod_depth = 0u;
        lfo->pitch_mod_sens = 0u;
    }
    slopsix_dx7s_lfo_tick(state);
    {
        int ok = 1;

        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            slopsix_lfo ref;

            memset(&ref, 0, sizeof(ref));
            ref.waveform = state->engines[v].lfo.waveform;
            ref.phase = state->engines[v].lfo.phase;
            slopsix_lfo_wave_step(&ref, 0u);
            if (state->engines[v].lfo.amplitude != ref.amplitude) {
                ok = 0;
            }
        }
        check(ok, "the per-voice LFOs use the shared wave recipes");
    }

    /* Each voice's delay fade follows its own accumulator/countdown. */
    for (v = 0; v < 2u; v++) {
        slopsix_lfo *lfo = &state->engines[v].lfo;

        lfo->delay_acc = 0u;
        lfo->fade = 0u;
        lfo->phase = 0u;
        lfo->phase_increment = 0u;
        lfo->waveform = 0u;
        lfo->delay_increment = (v == 0u)
            ? slopsix_lfo_delay_increment(0u)
            : slopsix_lfo_delay_increment(99u);
    }
    for (i = 0; i < 20u; i++) {
        slopsix_dx7s_lfo_tick(state);
    }
    check(state->engines[0].lfo.fade > 0u &&
          state->engines[1].lfo.fade == 0u,
          "each voice's delay fade runs on its own countdown");
    for (i = 0; i < 1100u; i++) {
        slopsix_dx7s_lfo_tick(state);
    }
    check(state->engines[0].lfo.fade == 0xFFu &&
          state->engines[1].lfo.fade > 0u,
          "the second voice's fade reaches full depth only after its own "
          "countdown");

    /* Note-on resets are per voice: sync seeds the keyed voice's phase,
     * and a non-zero delay clears only that voice's delay/fade pair. */
    state->patch.lfo_sync = true;
    state->patch.lfo_delay = 0u;
    state->engines[0].lfo.phase = 0x1234u;
    state->engines[1].lfo.phase = 0x5678u;
    slopsix_dx7s_lfo_note_on(state, &state->engines[0]);
    check(state->engines[0].lfo.phase == 0x7FFFu &&
          state->engines[1].lfo.phase == 0x5678u,
          "LFO sync seeds only the keyed voice's phase");

    state->patch.lfo_sync = false;
    state->patch.lfo_delay = 5u;
    state->engines[0].lfo.delay_acc = 123u;
    state->engines[0].lfo.fade = 45u;
    state->engines[1].lfo.delay_acc = 123u;
    state->engines[1].lfo.fade = 45u;
    slopsix_dx7s_lfo_note_on(state, &state->engines[0]);
    check(state->engines[0].lfo.delay_acc == 0u &&
          state->engines[0].lfo.fade == 0u &&
          state->engines[1].lfo.delay_acc == 123u &&
          state->engines[1].lfo.fade == 45u,
          "a patch delay clears only the keyed voice's fade countdown");

    /* The stagger: two voices keyed apart, with LFO sync on, run different
     * phases and therefore different amplitudes at the same sample. */
    state->patch.lfo_sync = true;
    state->patch.lfo_delay = 0u;
    for (v = 0; v < 2u; v++) {
        slopsix_lfo *lfo = &state->engines[v].lfo;

        lfo->phase = 0u;
        lfo->phase_increment = slopsix_lfo_speed_increment(50u);
        lfo->delay_increment = slopsix_lfo_delay_increment(99u);
        lfo->delay_acc = 0u;
        lfo->fade = 0u;
        lfo->sh_acc = 0u;
        lfo->amplitude = 0;
        lfo->waveform = 0u;
        lfo->pitch_mod_depth = 0u;
        lfo->amp_mod_depth = 0u;
        lfo->pitch_mod_sens = 0u;
    }
    slopsix_dx7s_lfo_note_on(state, &state->engines[0]);
    for (i = 0; i < 20u; i++) {
        slopsix_dx7s_lfo_tick(state);
    }
    slopsix_dx7s_lfo_note_on(state, &state->engines[1]);
    for (i = 0; i < 10u; i++) {
        slopsix_dx7s_lfo_tick(state);
    }
    check(state->engines[0].lfo.phase != state->engines[1].lfo.phase &&
          state->engines[0].lfo.amplitude !=
              state->engines[1].lfo.amplitude,
          "two voices keyed apart show the per-voice modulation stagger");

    /* Reset returns the personality state to its defaults. */
    slopsix_reset(ctx);
    {
        int zero = 1;

        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            if (state->engines[v].lfo.phase != 0u ||
                state->engines[v].lfo.delay_acc != 0u ||
                state->engines[v].lfo.fade != 0u ||
                state->engines[v].lfo.amplitude != 0) {
                zero = 0;
            }
        }
        check(state->lfo_mode == (uint8_t)SLOPSIX_LFO_SINGLE && zero,
              "reset parks every per-voice LFO and returns to Single mode");
    }
    slopsix_destroy_context(ctx);

    /* Neutral parity: in Single mode with the Mk1-equivalent parameters,
     * the DX7S pre-output render is the DX7 render bit for bit. */
    {
        slopsix_context dx7 = slopsix_create_context(SLOPSIX_ENGINE_DX7,
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        slopsix_context dx7s = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];

        check(dx7 != NULL && dx7s != NULL,
              "both contexts are created for the parity check");
        if (dx7 != NULL && dx7s != NULL) {
            make_init_patch(dump);
            /* Give the shared LFO real work in Single mode: pitch and
             * amplitude modulation with sync on. */
            dump[SLOPSIX_DUMP_LFO_SPEED] = 50u;
            dump[SLOPSIX_DUMP_LFO_DELAY] = 10u;
            dump[SLOPSIX_DUMP_LFO_PMD] = 50u;
            dump[SLOPSIX_DUMP_LFO_AMD] = 50u;
            dump[SLOPSIX_DUMP_LFO_SYNC] = 1u;
            dump[SLOPSIX_DUMP_LFO_WAVE] = 0u;
            dump[SLOPSIX_DUMP_LFO_PMS] = 7u;
            for (i = 0; i < 6u; i++) {
                dump[i * SLOPSIX_DUMP_OP_BLOCK + 14u] = 3u;
            }
            check(slopsix_set_voice(dx7, dump, sizeof(dump)) == SLOPSIX_OK &&
                  slopsix_set_voice(dx7s, dump, sizeof(dump)) == SLOPSIX_OK,
                  "both contexts take the reference patch");
            /* Compare the pre-output mixed bus: the personalities' output
             * stages differ by design (the DX7S has its own 16-bit DAC
             * tail), so the shared render is the invariant. */
            SLOPSIX_CONTEXT(dx7)->output_mix = parity_capture_mix;
            SLOPSIX_CONTEXT(dx7s)->output_mix = parity_capture_mix;
            slopsix_note_on(dx7, 69u, 100u);
            slopsix_note_on(dx7s, 69u, 100u);
            parity_dst = parity_bus_a;
            parity_len = 0;
            slopsix_process(dx7, render_buf_a, 512u);
            parity_dst = parity_bus_b;
            parity_len = 0;
            slopsix_process(dx7s, render_buf_b, 512u);
            {
                /* One voice is sounding, so the DX7's bus is exactly the
                 * compander applied to the DX7S's raw voice.  This checks
                 * the shared voice render and states the intended output
                 * delta: the DX7S omits the compander quantization. */
                size_t k;
                int equal = (parity_len == PARITY_BUS_MAX);

                for (k = 0; equal && k < PARITY_BUS_MAX; k++) {
                    slopsix_compander_word word;

                    slopsix_compander_quantize(parity_bus_b[k], &word);
                    if (slopsix_compander_value(&word) != parity_bus_a[k]) {
                        equal = 0;
                    }
                }
                check(equal,
                      "a neutral DX7S Single-mode render matches the DX7 "
                      "render before the compander bit for bit");
            }
        }
        slopsix_destroy_context(dx7);
        slopsix_destroy_context(dx7s);
    }

    /* The output level: the two personalities end in different DAC stages
     * (the Mk1's 12-bit companded grid, the DX7S's 16-bit PCM54HP), but
     * they must land on the same reference level, so a host that swaps one
     * engine for the other keeps its gain staging.  The voices differ
     * only by the compander's quantization, which is well under a
     * percent, so the peaks must agree to within a couple of percent. */
    {
        slopsix_context dx7 = slopsix_create_context(SLOPSIX_ENGINE_DX7,
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        slopsix_context dx7s = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];

        check(dx7 != NULL && dx7s != NULL,
              "both contexts are created for the level check");
        if (dx7 != NULL && dx7s != NULL) {
            int32_t peak_dx7;
            int32_t peak_dx7s;

            make_init_patch(dump);
            check(slopsix_set_voice(dx7, dump, sizeof(dump)) == SLOPSIX_OK &&
                  slopsix_set_voice(dx7s, dump, sizeof(dump)) == SLOPSIX_OK,
                  "both contexts take the reference patch");
            slopsix_note_on(dx7, 69u, 100u);
            slopsix_note_on(dx7s, 69u, 100u);
            slopsix_process(dx7, render_buf_a, 8000u);
            slopsix_process(dx7s, render_buf_b, 8000u);
            peak_dx7 = render_peak(render_buf_a, 8000u);
            peak_dx7s = render_peak(render_buf_b, 8000u);
            check(peak_dx7 > 1000 && peak_dx7s > 1000,
                  "both personalities render an audible level");
            check((peak_dx7 - peak_dx7s) * 50 <= peak_dx7 &&
                  (peak_dx7s - peak_dx7) * 50 <= peak_dx7,
                  "the DX7S output stage lands on the DX7's reference level");
        }
        slopsix_destroy_context(dx7);
        slopsix_destroy_context(dx7s);
    }
#endif
}

/*
 * The pitch-EG composition (D7S-4): the DX7S's range, velocity switch
 * and rate scaling, and the gate that keeps them out of the DX7.
 *
 * The stage checks the DX7S's own law against the shared one, and then
 * the gate: a DX7 context in a DX7S build must play the firmware's
 * envelope whatever the shared patch state holds, because the composition
 * is a personality hook and not a patch parameter the DX7 reads.
 */

/* The engine the last note-on keyed: the voices are allocated oldest
 * first, so the newest one is the one with the newest sequence. */
static slopsix_voice_engine *newest_engine(slopsix_context_state *state)
{
    slopsix_voice_engine *engine = &state->engines[0];
    unsigned i;

    for (i = 0; i < SLOPSIX_VOICE_COUNT; i++) {
        if (state->voices[i].seq == state->voice_seq) {
            engine = &state->engines[i];
        }
    }
    return engine;
}

static void stage_peg_range(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;

    printf("stage peg range\n");

    /* The DX7 personality: the firmware's plain envelope, whatever the
     * shared patch state holds. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7 context");
    if (ctx != NULL) {
        state = SLOPSIX_CONTEXT(ctx);

        check(state->pitch_eg_offset == slopsix_pitch_eg_offset,
              "the DX7 plays the firmware's plain pitch envelope");
#if defined(SLOPSIX_DX7S)
        /* The shared patch state carries the DX7S's parameters, so a
         * host that writes them straight into the shared state (rather
         * than through the personality-gated setters) must still not
         * change what the DX7 plays. */
        state->patch.peg_range = SLOPSIX_PEG_RANGE_HALF_OCTAVE;
        state->patch.peg_velocity = 1u;
        state->patch.peg_rate_scaling = 7u;
#endif
        slopsix_note_on(ctx, 60u, 100u);
        state = SLOPSIX_CONTEXT(ctx);
        newest_engine(state)->pitch_eg.level =
            SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&newest_engine(state)->pitch_eg) == -4096,
              "the DX7's pitch envelope ignores the DX7S parameters");
        slopsix_destroy_context(ctx);
    }

#if SLOPSIX_HAS_DX7S
    /* The DX7S personality: its own law composes the envelope. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    check(ctx != NULL, "create_context returns a DX7S context");
    if (ctx != NULL) {
        slopsix_voice_engine *engine;

        state = SLOPSIX_CONTEXT(ctx);

        check(state->pitch_eg_offset == slopsix_dx7s_pitch_eg_offset,
              "the DX7S plays its own composed pitch envelope");

        /* The neutral parameters leave the envelope as the firmware
         * plays it: the eight-octave range divides nothing and the
         * velocity switch is off. */
        slopsix_note_on(ctx, 60u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.range_shift == 0u &&
              engine->pitch_eg.velocity_on == 0u,
              "the DX7S's neutral parameters compose nothing");
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -4096,
              "the neutral DX7S envelope is the firmware's envelope");

        /* The range: the four settings in the hardware's data encoding,
         * raw 0 = 8 octaves dividing nothing and raw 3 = 1/2 octave
         * dividing by sixteen; the middle raw values scale 1/4 and 1/8. */
        slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_HALF_OCTAVE);
        slopsix_note_on(ctx, 60u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.range_shift == 4u,
              "the half-octave range divides the excursion by sixteen");
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -256,
              "the range divides the composed excursion");
        slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_2_OCTAVES);
        slopsix_note_on(ctx, 60u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.range_shift == 2u,
              "the two-octave range (raw 1) divides the excursion by four");
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -1024,
              "the range divides the composed excursion");
        slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_1_OCTAVE);
        slopsix_note_on(ctx, 60u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.range_shift == 3u,
              "the one-octave range (raw 2) divides the excursion by eight");
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -512,
              "the range divides the composed excursion");
        slopsix_set_peg_range(ctx, SLOPSIX_PEG_RANGE_8_OCTAVES);
        slopsix_note_on(ctx, 60u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.range_shift == 0u,
              "the eight-octave range divides nothing");
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -4096,
              "the eight-octave range is the firmware's span");

        /* The velocity switch: with it on, the depth follows the key's
         * velocity, and with it off the depth is the envelope's own. */
        slopsix_set_peg_velocity(ctx, true);
        slopsix_note_on(ctx, 60u, 127u);
        engine = newest_engine(state);
        check(engine->pitch_eg.velocity_on == 1u &&
              engine->pitch_eg.velocity_scale_q8 == 256u,
              "the velocity switch scales the depth with the key");
        slopsix_note_on(ctx, 60u, 0u);
        engine = newest_engine(state);
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == 0,
              "the velocity switch follows the key's velocity down to none");
        slopsix_set_peg_velocity(ctx, false);
        slopsix_note_on(ctx, 60u, 0u);
        engine = newest_engine(state);
        engine->pitch_eg.level = SLOPSIX_PITCH_EG_NEUTRAL_WORD - 4096;
        check(state->pitch_eg_offset(&engine->pitch_eg) == -4096,
              "the velocity switch off leaves the depth alone");

        /* The rate scaling: the stages' increments grow with the key. */
        slopsix_set_peg_rate_scaling(ctx, 0u);
        slopsix_note_on(ctx, 96u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.increment[0] ==
                  slopsix_pitch_eg_rate_value(0u),
              "without the rate scaling the stages keep the ROM's rates");
        slopsix_set_peg_rate_scaling(ctx, 7u);
        slopsix_note_on(ctx, 96u, 100u);
        engine = newest_engine(state);
        check(engine->pitch_eg.increment[0] == 2u,
              "the rate scaling speeds the stages up with the key");

        slopsix_destroy_context(ctx);
    }
#endif
}

/* ------------------------------------------- Phase D7S-3: unison modes */

#if SLOPSIX_HAS_DX7S
static int32_t unison_full_bus[PARITY_BUS_MAX];
static int32_t unison_sum_bus[PARITY_BUS_MAX];
static int32_t unison_one_bus[PARITY_BUS_MAX];
#endif

static void stage_unison(void)
{
    printf("stage unison\n");

#if SLOPSIX_HAS_DX7S
    {
        slopsix_context ctx;
        slopsix_context_state *state;
        uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];
        unsigned v;
        unsigned mode;

        ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7S,
            SLOPSIX_DEFAULT_SAMPLE_RATE);
        check(ctx != NULL, "create_context returns a DX7S context for the "
                           "unison checks");
        if (ctx == NULL) {
            return;
        }
        state = SLOPSIX_CONTEXT(ctx);
        make_init_patch(dump);
        check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK,
              "the unison checks load the reference patch");

        /* Unison Poly: four notes, four voices each, at the fixed groups. */
        check(slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_POLY) ==
              SLOPSIX_OK,
              "the key mode switches to Unison Poly");
        slopsix_note_on(ctx, 60u, 100u);
        check(state->voices[0].state == SLOPSIX_VOICE_ON &&
              state->voices[1].state == SLOPSIX_VOICE_ON &&
              state->voices[2].state == SLOPSIX_VOICE_ON &&
              state->voices[3].state == SLOPSIX_VOICE_ON &&
              state->voices[4].state == SLOPSIX_VOICE_FREE,
              "a Unison Poly key fills its four-voice group");
        check(state->unison_group_active[0] &&
              state->unison_group_note[0] == 60u,
              "the first group owns the key event");
        slopsix_note_on(ctx, 62u, 100u);
        slopsix_note_on(ctx, 64u, 100u);
        slopsix_note_on(ctx, 65u, 100u);
        check(count_voices(state, 60u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 62u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 64u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 65u, SLOPSIX_VOICE_ON) == 4,
              "Unison Poly caps at four notes of four voices");

        /* The fifth note steals the oldest group, all four voices, and
         * leaves the other groups alone. */
        slopsix_note_on(ctx, 67u, 100u);
        check(count_voices(state, 60u, SLOPSIX_VOICE_ON) == 0 &&
              state->voices[0].note == 67u &&
              state->voices[1].note == 67u &&
              state->voices[2].note == 67u &&
              state->voices[3].note == 67u &&
              state->voices[3].state == SLOPSIX_VOICE_ON,
              "the fifth Unison Poly note steals the oldest group");
        check(count_voices(state, 62u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 64u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 65u, SLOPSIX_VOICE_ON) == 4,
              "the steal leaves the other groups sounding");

        /* The steal is deterministic across a reset. */
        slopsix_reset(ctx);
        slopsix_set_voice(ctx, dump, sizeof(dump));
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_POLY);
        slopsix_note_on(ctx, 60u, 100u);
        slopsix_note_on(ctx, 62u, 100u);
        slopsix_note_on(ctx, 64u, 100u);
        slopsix_note_on(ctx, 65u, 100u);
        slopsix_note_on(ctx, 67u, 100u);
        check(state->voices[0].note == 67u &&
              state->voices[4].note == 62u &&
              state->voices[8].note == 64u &&
              state->voices[12].note == 65u,
              "the Unison Poly steal repeats identically after a reset");

        /* Note-off releases the group that owns the key; the key-off of
         * a stolen note no longer finds a group. */
        slopsix_note_off(ctx, 62u);
        check(count_voices(state, 62u, SLOPSIX_VOICE_RELEASED) == 4 &&
              count_voices(state, 62u, SLOPSIX_VOICE_ON) == 0,
              "a key-off releases every voice of its group");
        slopsix_note_off(ctx, 60u);
        check(count_voices(state, 67u, SLOPSIX_VOICE_ON) == 4,
              "the stolen note's old key-off releases nothing");

        /* Unison Mono: one sixteen-voice group, and a re-press steals
         * the group by continuing its envelopes. */
        slopsix_reset(ctx);
        slopsix_set_voice(ctx, dump, sizeof(dump));
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_MONO);
        slopsix_note_on(ctx, 60u, 100u);
        check(count_voices(state, 60u, SLOPSIX_VOICE_ON) ==
                  SLOPSIX_VOICE_COUNT,
              "Unison Mono stacks all sixteen voices on one note");
        slopsix_process(ctx, render_buf_a, 200u);
        {
            uint8_t stage_before = state->engines[0].eg[0].stage;
            int32_t level_before = state->engines[0].eg[0].level;

            slopsix_note_on(ctx, 64u, 100u);
            check(count_voices(state, 64u, SLOPSIX_VOICE_ON) ==
                      SLOPSIX_VOICE_COUNT &&
                  count_voices(state, 60u, SLOPSIX_VOICE_ON) == 0,
                  "a Unison Mono re-press steals the whole stack");
            check(state->engines[0].eg[0].stage == stage_before &&
                  state->engines[0].eg[0].level == level_before,
                  "the stolen stack continues its envelopes (Forced Damp "
                  "off)");
        }

        /* Detune: the provisional spread is symmetric, zero by default
         * and inactive outside the unison modes. */
        check(state->voices[0].unison_offset == 0 &&
              state->voices[SLOPSIX_VOICE_COUNT - 1].unison_offset == 0,
              "the neutral detune leaves the stack in tune");
        slopsix_set_unison_detune(ctx, 7u);
        check(state->voices[0].unison_offset < 0 &&
              state->voices[15].unison_offset > 0 &&
              state->voices[0].unison_offset ==
                  -state->voices[15].unison_offset,
              "the detune spreads the stack symmetrically");
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_POLY);
        check(state->voices[0].unison_offset == 0 &&
              state->voices[15].unison_offset == 0,
              "Poly mode carries no unison detune");

        /* The stack's mix is the bit-exact superposition of its voices. */
        slopsix_reset(ctx);
        slopsix_set_voice(ctx, dump, sizeof(dump));
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_MONO);
        slopsix_set_unison_detune(ctx, 0u);
        state->output_mix = parity_capture_mix;
        slopsix_note_on(ctx, 60u, 100u);
        parity_dst = unison_full_bus;
        parity_len = 0;
        slopsix_process(ctx, render_buf_a, PARITY_BUS_MAX);
        check(parity_len == PARITY_BUS_MAX,
              "the stack renders the captured bus samples");

        memset(unison_sum_bus, 0, sizeof(unison_sum_bus));
        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            unsigned w;

            slopsix_reset(ctx);
            state = SLOPSIX_CONTEXT(ctx);
            state->output_mix = parity_capture_mix;
            slopsix_set_voice(ctx, dump, sizeof(dump));
            slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_MONO);
            slopsix_set_unison_detune(ctx, 0u);
            slopsix_note_on(ctx, 60u, 100u);
            for (w = 0; w < SLOPSIX_VOICE_COUNT; w++) {
                if (w != v) {
                    state->voices[w].state = SLOPSIX_VOICE_FREE;
                }
            }
            parity_dst = unison_one_bus;
            parity_len = 0;
            slopsix_process(ctx, render_buf_b, PARITY_BUS_MAX);
            check(parity_len == PARITY_BUS_MAX,
                  "one stack voice renders the captured bus samples");
            for (w = 0; w < PARITY_BUS_MAX; w++) {
                unison_sum_bus[w] += unison_one_bus[w];
            }
        }
        check(memcmp(unison_full_bus, unison_sum_bus,
                     sizeof(unison_full_bus)) == 0,
              "the sixteen-voice stack is the bit-exact superposition of "
              "its voices");

        /* The pedal defers per group, all-notes-off acts as the poly
         * path's, and the groups clear at the end. */
        slopsix_reset(ctx);
        slopsix_set_voice(ctx, dump, sizeof(dump));
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_POLY);
        slopsix_note_on(ctx, 60u, 100u);
        slopsix_note_on(ctx, 62u, 100u);
        slopsix_set_sustain(ctx, true);
        slopsix_note_off(ctx, 60u);
        check(count_voices(state, 60u, SLOPSIX_VOICE_ON) == 4 &&
              !state->voices[0].held,
              "the pedal holds the released group");
        slopsix_all_notes_off(ctx);
        check(count_voices(state, 60u, SLOPSIX_VOICE_ON) == 4 &&
              count_voices(state, 62u, SLOPSIX_VOICE_ON) == 4,
              "all-notes-off under the pedal releases nothing");
        check(!state->unison_group_active[0] &&
              !state->unison_group_active[1],
              "all-notes-off clears the groups");
        slopsix_set_sustain(ctx, false);
        check(count_voices(state, 60u, SLOPSIX_VOICE_RELEASED) == 4 &&
              count_voices(state, 62u, SLOPSIX_VOICE_RELEASED) == 4,
              "pedal-up releases every voice the pedal held");

        slopsix_reset(ctx);
        slopsix_set_voice(ctx, dump, sizeof(dump));
        slopsix_set_key_mode(ctx, SLOPSIX_KEY_UNISON_POLY);
        slopsix_note_on(ctx, 60u, 100u);
        slopsix_note_on(ctx, 62u, 100u);
        slopsix_all_notes_off(ctx);
        check(count_voices(state, 60u, SLOPSIX_VOICE_RELEASED) == 4 &&
              count_voices(state, 62u, SLOPSIX_VOICE_RELEASED) == 4,
              "all-notes-off without the pedal releases every group");

        /* Reset determinism at all four key modes. */
        {
            static const slopsix_key_mode modes[4] = {
                SLOPSIX_KEY_POLY, SLOPSIX_KEY_MONO,
                SLOPSIX_KEY_UNISON_POLY, SLOPSIX_KEY_UNISON_MONO
            };

            for (mode = 0; mode < 4u; mode++) {
                slopsix_reset(ctx);
                slopsix_set_voice(ctx, dump, sizeof(dump));
                slopsix_set_key_mode(ctx, modes[mode]);
                slopsix_note_on(ctx, 60u, 100u);
                slopsix_note_on(ctx, 62u, 100u);
                slopsix_process(ctx, render_buf_a, 512u);

                slopsix_reset(ctx);
                slopsix_set_voice(ctx, dump, sizeof(dump));
                slopsix_set_key_mode(ctx, modes[mode]);
                slopsix_note_on(ctx, 60u, 100u);
                slopsix_note_on(ctx, 62u, 100u);
                slopsix_process(ctx, render_buf_b, 512u);
                check(memcmp(render_buf_a, render_buf_b,
                             512u * 2u * sizeof(int16_t)) == 0,
                      "every key mode renders identically after a reset");
            }
        }

        check(state->key_mode == (uint8_t)SLOPSIX_KEY_POLY ||
              state->key_mode == (uint8_t)SLOPSIX_KEY_UNISON_MONO,
              "the key mode survives the last reset's explicit set");
        slopsix_destroy_context(ctx);
    }
#else
    printf("  (the DX7S personality is absent from this build)\n");
#endif
}

static void stage_blockgraph(void)
{
    unsigned algorithm;

    printf("stage blockgraph\n");

    for (algorithm = 0; algorithm < 32u; algorithm++) {
        const slopsix_algorithm *alg = slopsix_algorithm_get(algorithm);

        check(alg->schedule.fast != 0u,
              "the checked-in algorithm carries a fast schedule");
        block_check_algorithm(algorithm, 0);
        block_check_algorithm(algorithm, 1);
    }
}

/* The block render path's driver-level reference: the render call as it
 * ran per sample, kept here so the sub-block cutting, the tick handling
 * and the bus stage can be checked against it through the public API. */
static void process_reference(slopsix_context ctx, int16_t *stereo_out,
                              size_t frames)
{
    slopsix_context_state *state = SLOPSIX_CONTEXT(ctx);
    const slopsix_algorithm *alg;
    size_t sample;
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

    for (sample = 0; sample < frames; sample++) {
        int32_t bus = 0;
        int lfo_tick;
        int peg_tick;
        uint16_t porta_changed = 0;
        unsigned v;

        lfo_tick = slopsix_lfo_cadence_tick(&state->lfo_cadence);
        peg_tick = slopsix_pitch_eg_cadence_tick(&state->pitch_cadence);

        if (lfo_tick) {
            slopsix_lfo_tick(state);
            porta_changed = slopsix_porta_process(state, state->porta_bank);
            state->porta_bank ^= 1u;
        }
        if (lfo_tick || peg_tick) {
            int32_t lfo_offset = slopsix_mod_pitch_offset(state);

            for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
                int32_t offset;

                if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                    continue;
                }
                if (lfo_tick) {
                    slopsix_voice_engine_update_ams(state,
                        &state->engines[v]);
                }
                if (peg_tick) {
                    slopsix_pitch_eg_update(&state->engines[v].pitch_eg);
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

        for (v = 0; v < SLOPSIX_VOICE_COUNT; v++) {
            slopsix_compander_word word;
            int32_t out;

            if (state->voices[v].state == SLOPSIX_VOICE_FREE) {
                continue;
            }

            slopsix_voice_render_sample(&state->engines[v], alg, &out);
            slopsix_compander_quantize(out, &word);
            bus += slopsix_compander_value(&word);
        }

        slopsix_output_mix(state, bus,
            &stereo_out[sample * 2u], &stereo_out[sample * 2u + 1u]);
    }
}

/* The block-process stage's patch: every operator audible so the whole
 * graph is live, the feedback up so the pair's paths carry, and the LFO
 * depths up so the ticks rewrite both registers. */
static void make_block_patch(uint8_t *dump, uint8_t algorithm)
{
    unsigned block;

    memset(dump, 0, SLOPSIX_VOICE_DUMP_SIZE);

    for (block = 0; block < 6u; block++) {
        uint8_t *op = dump + block * SLOPSIX_DUMP_OP_BLOCK;

        op[0] = 70u; op[1] = 60u; op[2] = 50u; op[3] = 40u;
        op[4] = 60u; op[5] = 50u; op[6] = 70u; op[7] = 0u;
        op[16] = 70u + (uint8_t)(block * 5u);
        op[18] = (uint8_t)(1u + (block & 3u));
        op[20] = (uint8_t)(1u + block);
        op[13] = (uint8_t)(block & 3u);      /* the rate sensitivity */
        op[15] = (uint8_t)((block * 7u) & 7u);  /* velocity sensitivity */
        op[17] = (uint8_t)(block & 3u);      /* the amp-mod sensitivity */
    }

    dump[126] = 99u; dump[127] = 99u; dump[128] = 99u; dump[129] = 99u;
    dump[130] = 30u; dump[131] = 70u; dump[132] = 50u; dump[133] = 99u;
    dump[SLOPSIX_DUMP_ALGORITHM] = algorithm;
    dump[SLOPSIX_DUMP_FEEDBACK] = 5u;
    dump[SLOPSIX_DUMP_OSC_SYNC] = 1u;
    dump[SLOPSIX_DUMP_LFO_SPEED] = 33u;
    dump[SLOPSIX_DUMP_LFO_DELAY] = 40u;
    dump[SLOPSIX_DUMP_LFO_PMD] = 60u;
    dump[SLOPSIX_DUMP_LFO_AMD] = 80u;
    dump[SLOPSIX_DUMP_LFO_WAVE] = 0u;
    dump[SLOPSIX_DUMP_LFO_PMS] = 3u;
    dump[SLOPSIX_DUMP_TRANSPOSE] = 24u;
}

/* The total the process stage renders, and where its events land (the
 * positions are the chunk boundaries every split pattern honours). */
#define BLOCK_TOTAL 2048
#define BLOCK_EVENT1 512
#define BLOCK_EVENT2 1024
#define BLOCK_EVENT3 1536

/* One run: the same event timeline through both render calls, the
 * buffer split as the caller asks.  `splits` is the pattern the
 * driver cycles over the four 512-sample segments between events. */
static void block_run_split(slopsix_context ctx_ref, slopsix_context ctx_blk,
                            const unsigned *splits, unsigned nsplits,
                            int16_t *buf_ref, int16_t *buf_blk,
                            int *mismatch)
{
    static const size_t segments[4] = { BLOCK_EVENT1,
                                        BLOCK_EVENT2 - BLOCK_EVENT1,
                                        BLOCK_EVENT3 - BLOCK_EVENT2,
                                        BLOCK_TOTAL - BLOCK_EVENT3 };
    unsigned seg;
    unsigned idx = 0;

    slopsix_note_on(ctx_ref, 60, 100);
    slopsix_note_on(ctx_blk, 60, 100);

    for (seg = 0; seg < 4u; seg++) {
        size_t left = segments[seg];
        unsigned which = 0;

        while (left > 0) {
            size_t n = splits[which % nsplits];

            if (n > left) {
                n = left;
            }

            memset(buf_ref + idx * 2u, 0x55, n * 2u * 2u);
            memset(buf_blk + idx * 2u, 0x55, n * 2u * 2u);
            process_reference(ctx_ref, buf_ref + idx * 2u, n);
            slopsix_process(ctx_blk, buf_blk + idx * 2u, n);
            if (memcmp(buf_ref + idx * 2u, buf_blk + idx * 2u,
                       n * 2u * 2u) != 0) {
                *mismatch = 1;
            }
            idx += n;
            left -= n;
            which++;
        }

        /* The events land at the segment boundaries, the same absolute
         * samples in both runs. */
        if (seg == 0u) {
            slopsix_set_pitch_bend(ctx_ref, 0x3FFF);
            slopsix_set_pitch_bend(ctx_blk, 0x3FFF);
            slopsix_set_ctrl_value(ctx_ref, SLOPSIX_CTRL_MOD_WHEEL, 60);
            slopsix_set_ctrl_value(ctx_blk, SLOPSIX_CTRL_MOD_WHEEL, 60);
        } else if (seg == 1u) {
            slopsix_set_ctrl_value(ctx_ref, SLOPSIX_CTRL_FOOT, 127);
            slopsix_set_ctrl_value(ctx_blk, SLOPSIX_CTRL_FOOT, 127);
        } else if (seg == 2u) {
            slopsix_note_off(ctx_ref, 60);
            slopsix_note_off(ctx_blk, 60);
            slopsix_note_on(ctx_ref, 67, 64);
            slopsix_note_on(ctx_blk, 67, 64);
        }
    }
}

static void stage_blockprocess(void)
{
    static const unsigned patterns[][6] = {
        { 512, 0, 0, 0, 0, 0 },
        { 64, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0, 0, 0 },
        { 63, 1, 0, 0, 0, 0 },
        { 65, 63, 0, 0, 0, 0 },
        { 7, 0, 0, 0, 0, 0 },
        { 128, 129, 128, 127, 0, 0 },
        { 200, 0, 0, 0, 0, 0 },
    };
    int16_t buf_ref[BLOCK_TOTAL * 2u];
    int16_t buf_blk[BLOCK_TOTAL * 2u];
    uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];
    unsigned algorithm;
    unsigned p;

    printf("stage blockprocess\n");

    for (algorithm = 0; algorithm < 32u; algorithm++) {
        for (p = 0; p < 8u; p++) {
            unsigned nsplits;
            slopsix_context ctx_ref;
            slopsix_context ctx_blk;
            int mismatch = 0;

            for (nsplits = 0; nsplits < 6u; nsplits++) {
                if (patterns[p][nsplits] == 0u) {
                    break;
                }
            }

            make_block_patch(dump, (uint8_t)algorithm);
            ctx_ref = slopsix_create_context(SLOPSIX_ENGINE_DX7,
                                             SLOPSIX_DEFAULT_SAMPLE_RATE);
            ctx_blk = slopsix_create_context(SLOPSIX_ENGINE_DX7,
                                             SLOPSIX_DEFAULT_SAMPLE_RATE);
            check(slopsix_set_voice(ctx_ref, dump, sizeof(dump)) ==
                      SLOPSIX_OK &&
                  slopsix_set_voice(ctx_blk, dump, sizeof(dump)) ==
                      SLOPSIX_OK,
                  "the block-process pair takes the patch");

            block_run_split(ctx_ref, ctx_blk, patterns[p], nsplits,
                            buf_ref, buf_blk, &mismatch);

            if (!block_probe_equal(&SLOPSIX_CONTEXT(ctx_ref)->engines[0],
                                   &SLOPSIX_CONTEXT(ctx_blk)->engines[0])) {
                mismatch = 1;
            }

            check(!mismatch,
                  "the block process call matches the per-sample loop "
                  "for the run's splits");
            if (mismatch) {
                printf("  algorithm %u, pattern %u\n", algorithm + 1u,
                       p + 1u);
            }

            slopsix_destroy_context(ctx_ref);
            slopsix_destroy_context(ctx_blk);
        }
    }

    /* The tick-phase sweep: the sub-blocks must cut at the ticks
     * wherever they land inside a call.  The prelude advances both
     * contexts one sample at a time up to offset k, then the rest of
     * the take renders in 64-frame calls (the block context) against
     * continued single-frame calls (the reference); every tick position
     * inside a block passes through this. */
    for (algorithm = 0; algorithm < 32u; algorithm += 5u) {
        unsigned k;
        unsigned take = 768u;

        make_block_patch(dump, (uint8_t)algorithm);
        for (k = 0; k < 140u; k++) {
            slopsix_context ctx_ref;
            slopsix_context ctx_blk;
            int mismatch = 0;
            unsigned i;
            unsigned idx = 0;

            ctx_ref = slopsix_create_context(SLOPSIX_ENGINE_DX7,
                                             SLOPSIX_DEFAULT_SAMPLE_RATE);
            ctx_blk = slopsix_create_context(SLOPSIX_ENGINE_DX7,
                                             SLOPSIX_DEFAULT_SAMPLE_RATE);
            slopsix_set_voice(ctx_ref, dump, sizeof(dump));
            slopsix_set_voice(ctx_blk, dump, sizeof(dump));
            slopsix_note_on(ctx_ref, 48, 90);
            slopsix_note_on(ctx_blk, 48, 90);

            for (i = 0; i < k; i++) {
                process_reference(ctx_ref, buf_ref + idx * 2u, 1);
                slopsix_process(ctx_blk, buf_blk + idx * 2u, 1);
                idx++;
            }
            while (idx < take) {
                size_t n = (take - idx > 64u) ? 64u : take - idx;
                size_t t;

                for (t = 0; t < n; t++) {
                    process_reference(ctx_ref, buf_ref + (idx + t) * 2u, 1);
                }
                slopsix_process(ctx_blk, buf_blk + idx * 2u, n);
                idx += n;
            }

            if (memcmp(buf_ref, buf_blk, take * 2u * 2u) != 0) {
                mismatch = 1;
            }
            if (!block_probe_equal(&SLOPSIX_CONTEXT(ctx_ref)->engines[0],
                                   &SLOPSIX_CONTEXT(ctx_blk)->engines[0])) {
                mismatch = 1;
            }
            check(!mismatch,
                  "the block process call matches the per-sample loop at "
                  "every tick position");
            if (mismatch) {
                printf("  algorithm %u, tick offset %u\n", algorithm + 1u,
                       k);
            }

            slopsix_destroy_context(ctx_ref);
            slopsix_destroy_context(ctx_blk);
        }
    }
}

/* -------------------------------------------------------- the LFO */

static void stage_lfo(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    slopsix_lfo *lfo;
    uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];
    slopsix_op_params params;
    int intervals[8];
    int updates;
    int previous;
    int i;
    unsigned char lfo_seen[4200];
    unsigned char peg_seen[4200];

    printf("stage lfo\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);
    lfo = &state->lfo;

    /* The speed law: algebraic, with the zero speed's accumulator seed.
     * 660 * v >> 8 times its own multiplier; the samples come from the
     * law's own steps (the kinks sit at the speed whose high byte passes
     * 160). */
    check(slopsix_lfo_speed_increment(0u) == 11u,
          "speed 0 seeds the accumulator with 11");
    check(slopsix_lfo_speed_increment(1u) == 22u &&
          slopsix_lfo_speed_increment(3u) == 77u,
          "the low speeds are the high byte times eleven");
    check(slopsix_lfo_speed_increment(10u) == 275u &&
          slopsix_lfo_speed_increment(62u) == 1749u,
          "the speeds below the kink follow the law");
    check(slopsix_lfo_speed_increment(63u) == 1782u &&
          slopsix_lfo_speed_increment(64u) == 1980u,
          "the law's multiplier grows past the high byte's 160");
    check(slopsix_lfo_speed_increment(99u) == 8670u,
          "speed 99 is the law's top (255 * 34)");

    /* The delay law: the 99-minus-delay value's nibbles. */
    check(slopsix_lfo_delay_increment(99u) == 64u,
          "delay 99 is the slowest delay counter");
    check(slopsix_lfo_delay_increment(0u) == 4864u,
          "delay 0 is the fastest delay counter");
    check(slopsix_lfo_delay_increment(95u) == 80u &&
          slopsix_lfo_delay_increment(60u) == 368u &&
          slopsix_lfo_delay_increment(16u) == 2432u,
          "the delay law follows the nibbles");

    /* The patch activation: the dump bytes through the scaling. */
    memset(dump, 0, sizeof(dump));
    dump[SLOPSIX_DUMP_LFO_SPEED] = 50u;
    dump[SLOPSIX_DUMP_LFO_DELAY] = 50u;
    dump[SLOPSIX_DUMP_LFO_PMD] = 99u;
    dump[SLOPSIX_DUMP_LFO_AMD] = 99u;
    dump[SLOPSIX_DUMP_LFO_WAVE] = 3u;
    dump[SLOPSIX_DUMP_LFO_PMS] = 7u;
    slopsix_lfo_patch_activate(state, dump);
    check(lfo->phase_increment == 1408u,
          "speed 50 parses to the law's increment");
    check(lfo->delay_increment == 544u,
          "delay 50 parses to the law's increment");
    check(lfo->pitch_mod_depth == 255u && lfo->amp_mod_depth == 255u,
          "the depths are the value scale's high bytes");
    check(lfo->waveform == 3u && lfo->pitch_mod_sens == 255u,
          "the wave byte passes raw and the sensitivity takes the table");
    check(state->patch.lfo_delay == 50u && state->patch.lfo_sync == 0u,
          "the note-on path's own bytes stay raw");
    dump[SLOPSIX_DUMP_LFO_SYNC] = 5u;
    dump[SLOPSIX_DUMP_LFO_PMS] = 200u;
    dump[SLOPSIX_DUMP_LFO_SPEED] = 255u;
    slopsix_lfo_patch_activate(state, dump);
    check(state->patch.lfo_sync != 0u, "any sync byte value enables sync");
    check(lfo->pitch_mod_sens == 255u,
          "the sensitivity clamps to the table's last entry");
    check(lfo->phase_increment == 8670u,
          "the speed clamps to the law's top");

    /* The waveforms.  The phase is held (a zero increment) so each tick
     * reads one exact phase. */
    lfo->phase_increment = 0u;
    lfo->waveform = 0u;                    /* triangle */
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the triangle starts at its trough");
    lfo->phase = 0x4000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 0, "the triangle's quarter is neutral");
    lfo->phase = 0x7F80u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 127, "the triangle peaks before the midpoint");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 127,
          "the triangle rides its peak across the midpoint");
    lfo->phase = 0xC000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -1, "the triangle's last quarter falls");
    lfo->phase = 0xFF80u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the triangle closes its cycle");

    lfo->waveform = 2u;                    /* saw up */
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 0, "the rising saw reads the high byte");
    lfo->phase = 0x7F80u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 127, "the rising saw peaks at the midpoint");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the rising saw resets at the midpoint");

    lfo->waveform = 1u;                    /* saw down */
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -1, "the falling saw is the complement");
    lfo->phase = 0x7F80u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the falling saw troughs at the midpoint");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 127, "the falling saw resets at the midpoint");

    lfo->waveform = 3u;                    /* square */
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 127, "the square's first half is positive");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the square's second half is negative");

    lfo->waveform = 4u;                    /* sine */
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 2, "the sine starts at its table's first entry");
    lfo->phase = 0x2000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 91, "the sine reads the table directly");
    lfo->phase = 0x6000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 89, "the sine mirrors its index");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -3, "the sine inverts its negative half");
    lfo->phase = 0xC000u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -128, "the sine troughs");

    lfo->waveform = 5u;                    /* sample and hold */
    lfo->phase = 0x7F00u;
    lfo->phase_increment = 0x0100u;        /* the add crosses 0x7FFF */
    lfo->sh_acc = 0u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 11, "the S&H resamples on the phase's overflow");
    lfo->phase = 0x8000u;                  /* the next add does not overflow */
    slopsix_lfo_tick(state);
    check(lfo->amplitude == 11, "the S&H holds between resamples");
    lfo->phase = 0x7F80u;
    slopsix_lfo_tick(state);
    check(lfo->amplitude == -68,
          "the S&H's LCG steps its holder (179x + 11)");
    lfo->phase_increment = 0u;

    /* The delay and the fade: delay 0's increment overflows the 16-bit
     * accumulator on the fourteenth tick and locks it; the fade then
     * grows by the increment's high byte (19) per tick. */
    memset(dump, 0, sizeof(dump));
    dump[SLOPSIX_DUMP_LFO_SPEED] = 50u;
    dump[SLOPSIX_DUMP_LFO_DELAY] = 0u;
    dump[SLOPSIX_DUMP_LFO_PMD] = 99u;
    dump[SLOPSIX_DUMP_LFO_AMD] = 99u;
    dump[SLOPSIX_DUMP_LFO_WAVE] = 3u;
    dump[SLOPSIX_DUMP_LFO_PMS] = 7u;
    slopsix_lfo_patch_activate(state, dump);
    lfo->phase_increment = 0u;
    lfo->phase = 0x8000u;                  /* the square's negative half */
    lfo->delay_acc = 0u;                   /* a fresh counter for the drive */
    lfo->fade = 0u;
    for (i = 1; i <= 13; i++) {
        slopsix_lfo_tick(state);
    }
    check(lfo->fade == 0u,
          "the delay accumulator has not overflowed in 13 ticks");
    slopsix_lfo_tick(state);
    check(lfo->fade == 19u && lfo->delay_acc == 0xFFFFu,
          "the overflow tick locks the accumulator and starts the fade");
    for (i = 14; i < 27; i++) {
        slopsix_lfo_tick(state);
    }
    check(lfo->fade == 255u, "the fade fills in thirteen more ticks");

    /* The amp-mod register: the inverted-polarity wave form.  With the
     * square's negative half and the full factor ((255*255)>>8 = 254,
     * times the inverted wave's 255 >> 8 = 253), the register sits at the
     * table's third entry; the positive half gives none. */
    check(lfo->amp_mod_register == 0xE0u,
          "the amp-mod register follows the table's third entry");
    lfo->phase = 0x0000u;                  /* the positive half */
    slopsix_lfo_tick(state);
    check(lfo->amp_mod_register == 0u,
          "the amp-mod route uses the wave's inverted polarity");

    /* The pitch-modification word: the factor, the sensitivity-scaled
     * wave and the halving.  At the full depth the negative half swings
     * the word down and the positive half up. */
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->pitch_mod_word == -16129,
          "the full pitch term is the product's signed halving");
    check(slopsix_mod_pitch_offset(state) == -4033,
          "the word converts to the engine's floor division");
    lfo->phase = 0x0000u;
    slopsix_lfo_tick(state);
    check(lfo->pitch_mod_word == 16002 && lfo->amplitude == 127,
          "the positive half swings the word up");
    check(slopsix_mod_pitch_offset(state) == 4000,
          "the upward conversion floors the same way");

    /* The frequency path: the composed word moves a ratio operator; a
     * fixed-frequency one ignores it; and the pitch-EG offset composes
     * into the same word beside it. */
    params.mode = 0u;
    params.coarse = 1u;
    params.fine = 0u;
    params.detune = 7u;
    check(slopsix_op_freq_word(69, &params, 0, 4000) ==
          slopsix_op_freq_word(69, &params, 0, 0) + 4000,
          "the LFO's converted word is one octave per 4096 units");
    params.mode = 1u;
    check(slopsix_op_freq_word(69, &params, 0, -4033) ==
          slopsix_op_freq_word(69, &params, 0, 0),
          "a fixed-frequency operator ignores the LFO");
    params.mode = 0u;
    check(slopsix_op_freq_word(69, &params, 0,
                               slopsix_mod_pitch_offset(state) + 4096) ==
          slopsix_op_freq_word(69, &params, 0,
                               slopsix_mod_pitch_offset(state)) + 4096,
          "the LFO and the pitch EG share one log-frequency word");

    /* The note-on hook: delay reset only with a delay, sync seeding only
     * with sync. */
    lfo->phase = 0x1234u;
    lfo->delay_acc = 5000u;
    lfo->fade = 9u;
    state->patch.lfo_delay = 0u;
    state->patch.lfo_sync = 0u;
    slopsix_lfo_note_on(state);
    check(lfo->phase == 0x1234u && lfo->delay_acc == 5000u && lfo->fade == 9u,
          "a delay of 0 resets neither the delay nor the phase");
    state->patch.lfo_delay = 50u;
    slopsix_lfo_note_on(state);
    check(lfo->phase == 0x1234u && lfo->delay_acc == 0u && lfo->fade == 0u,
          "a delay clears the delay accumulator and the fade");
    state->patch.lfo_sync = 1u;
    slopsix_lfo_note_on(state);
    check(lfo->phase == 0x7FFFu, "sync seeds the phase at 0x7FFF");

    /* The cadence: one tick every 785/6 anchor-rate samples, five
     * intervals of 131 and one of 130 per cycle; and at twice the anchor
     * rate the first interval is double. */
    {
        slopsix_lfo_cadence cadence;

        slopsix_lfo_cadence_init(&cadence, SLOPSIX_DEFAULT_SAMPLE_RATE);
        updates = 0;
        previous = 0;
        for (i = 1; i <= 1000; i++) {
            if (slopsix_lfo_cadence_tick(&cadence)) {
                if (updates < 6) {
                    intervals[updates] = i - previous;
                }
                previous = i;
                updates++;
            }
        }
        check(updates >= 6 && intervals[0] == 131 && intervals[1] == 131 &&
              intervals[2] == 131 && intervals[3] == 131 &&
              intervals[4] == 131 && intervals[5] == 130,
              "the LFO ticks at the 375.3 Hz staircase");
        check(intervals[0] + intervals[1] + intervals[2] + intervals[3] +
              intervals[4] + intervals[5] == 785,
              "six ticks span exactly 785 samples");

        for (i = 0; i < 6; i++) {
            intervals[i] = 0;
        }
        slopsix_lfo_cadence_init(&cadence,
                                 SLOPSIX_DEFAULT_SAMPLE_RATE * 2u);
        for (i = 1; i <= 600; i++) {
            if (slopsix_lfo_cadence_tick(&cadence)) {
                break;
            }
        }
        check(i == 262, "the cadence scales with the host sample rate");

        /* The two cadences are phase-locked: the pitch EG's gate (the
         * doubled period) lands exactly on every second LFO tick. */
        {
            slopsix_lfo_cadence lfo_c;
            slopsix_pitch_eg_cadence peg_c;
            int locked = 1;
            int lfo_fires = 0;
            int peg_fires = 0;

            slopsix_lfo_cadence_init(&lfo_c, SLOPSIX_DEFAULT_SAMPLE_RATE);
            slopsix_pitch_eg_cadence_init(&peg_c,
                                          SLOPSIX_DEFAULT_SAMPLE_RATE);
            memset(lfo_seen, 0, sizeof(lfo_seen));
            memset(peg_seen, 0, sizeof(peg_seen));
            for (i = 1; i < 4200; i++) {
                if (slopsix_lfo_cadence_tick(&lfo_c)) {
                    lfo_seen[i] = 1;
                    lfo_fires++;
                }
                if (slopsix_pitch_eg_cadence_tick(&peg_c)) {
                    peg_seen[i] = 1;
                    peg_fires++;
                    if (!lfo_seen[i]) {
                        locked = 0;
                    }
                }
            }
            check(locked && lfo_fires == 32 && peg_fires == 16,
                  "the pitch EG walks on every second LFO tick");
        }
    }

    /* The context creation carries both cadences to a non-default host
     * rate, and they stay phase-locked there: the pitch EG's gate lands
     * on every second LFO tick at any rate (one shared threshold, the
     * pitch EG's step half of the LFO's). */
    {
        slopsix_context host_ctx;
        slopsix_context_state *host_state;
        int lfo_fires = 0;
        int peg_fires = 0;
        int locked = 1;

        host_ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, 44100u);
        if (host_ctx == NULL) {
            check(0, "create_context returns a context");
        } else {
            host_state = SLOPSIX_CONTEXT(host_ctx);
            check(host_state->sample_rate == 44100u &&
                  host_state->lfo_cadence.sample_rate == 44100u &&
                  host_state->pitch_cadence.sample_rate == 44100u,
                  "create_context takes the requested rate for both"
                  " cadences");

            memset(lfo_seen, 0, sizeof(lfo_seen));
            for (i = 1; i < 4200; i++) {
                if (slopsix_lfo_cadence_tick(&host_state->lfo_cadence)) {
                    lfo_seen[i] = 1;
                    lfo_fires++;
                }
                if (slopsix_pitch_eg_cadence_tick(
                        &host_state->pitch_cadence)) {
                    peg_fires++;
                    if (!lfo_seen[i]) {
                        locked = 0;
                    }
                }
            }
            check(locked && lfo_fires > 0 && peg_fires > 0 &&
                  lfo_fires <= 2 * peg_fires + 1 &&
                  2 * peg_fires <= lfo_fires,
                  "the creation-rate cadences stay phase-locked");
            slopsix_destroy_context(host_ctx);
        }
    }

    slopsix_destroy_context(ctx);
}

/* ------------------------------------------------------------ controllers */

/*
 * The controller stage (research fw MOD_PROCESS_INPUT_SOURCES, ROM $E208;
 * MOD_AMP_LOAD_TO_EGS and MOD_PITCH_LOAD_TO_EGS per tick).
 *
 * The LFO is frozen per case: phase increments 0 (a stable wave), the
 * delay increment 0 with the accumulator at its ceiling (a stable fade),
 * and a wave phase the tests set per case.  Both registers' compositions
 * are recomputed in the stage from the algorithm page's recipes and the
 * checked-in tables, over the controller-route matrix.
 */
static void stage_ctrl(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    slopsix_lfo *lfo;
    unsigned amp_depth;
    unsigned pitch_depth;

    printf("stage ctrl\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);
    lfo = &state->lfo;

    /* The freeze: a stable wave and a stable fade, the square wave's
     * amplitude following the phase's top bit only. */
    lfo->waveform = 3u;
    lfo->phase_increment = 0u;
    lfo->delay_increment = 0u;
    lfo->delay_acc = 0xFFFFu;
    lfo->fade = 0xFFu;

    /* The defaults: nothing routed, so the cached factors and the
     * EG-bias total are all zero, and the register composition runs
     * without any controller share. */
    check(state->mod_amp_factor == 0u && state->mod_eg_bias_inv == 0u &&
          state->ctrl_scaled_input[0] == 0u &&
          state->ctrl_scaled_input[1] == 0u &&
          state->ctrl_scaled_input[2] == 0u &&
          state->ctrl_scaled_input[3] == 0u,
          "the reset context carries the neutral controller arithmetic");

    /* The scaled input: the analog input times the range's scaled high
     * byte (660*range)>>8, MUL-style, the high byte kept. */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_MOD_WHEEL,
                            SLOPSIX_ASSIGN_AMP);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 50u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 127u);
    check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 63u,
          "the scaled input is the mul pair's high byte at range 50");
    check(state->mod_amp_factor == 63u,
          "the amp factor carries the amp-routed source's scaled input");
    check(state->mod_eg_bias_inv == 0u,
          "an amp route alone leaves the EG-bias total at its neutral");

    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 0u &&
          state->mod_amp_factor == 0u,
          "a zero input leaves no scaled input and no amp factor");

    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 127u);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 1u);
    check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 0u &&
          state->mod_amp_factor == 0u,
          "the small range's 2-unit high byte floors the scaled input away");

    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 99u);
    check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 126u,
          "the full range scales the full input to 126 of 127");
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 64u);
    check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 63u,
          "the mid input scales to the value scale's own quantisation");

    /* The amp-mod register through the route (research V-03): the amp
     * factor joins the LFO's depth term before the byte's clamp, and the
     * EG-bias total folds in its inverted domain.  With the square's
     * negative half the wave form is 255, so the multiply keeps almost
     * all of the value; with the positive half it is 0 and only the
     * folds survive to the table. */
    amp_depth = 0xFFu;
    lfo->amp_mod_depth = (uint8_t)amp_depth;

    /* Nothing routed: the register is the LFO's own composition. */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    {
        unsigned factor = (amp_depth * 0xFFu) >> 8;
        unsigned wave = 255u;   /* the square's negative half */

        lfo->phase = 0x8000u;
        slopsix_lfo_tick(state);
        check(lfo->amp_mod_register ==
                  slopsix_amp_mod_table[0xFFu - ((factor * wave) >> 8)],
              "no routes: the register is the LFO's own composition");
    }

    /* The amp route: the source's scaled input raises the factor. */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_MOD_WHEEL,
                            SLOPSIX_ASSIGN_AMP);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 99u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 127u);
    {
        unsigned base = ((amp_depth * 0xFFu) >> 8) + 126u;
        unsigned factor = base > 0xFFu ? 0xFFu : base;
        unsigned wave = 255u;   /* the square's negative half */

        lfo->phase = 0x8000u;
        slopsix_lfo_tick(state);
        check(lfo->amp_mod_register ==
                  slopsix_amp_mod_table[0xFFu - ((factor * wave) >> 8)],
              "the amp-routed factor joins the LFO's depth term");
    }

    /* The EG-bias route, the composition's inverted domain.  With no
     * input the total is the range's own scaled value; pushing the
     * controller moves the register the loud way (less attenuation). */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_MOD_WHEEL,
                            SLOPSIX_ASSIGN_EG);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 20u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    check(state->mod_eg_bias_inv == 51u &&
          state->mod_amp_factor == 0u,
          "the EG-bias total is the range's own scaled value at rest");

    lfo->amp_mod_depth = 0u;
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amp_mod_register == slopsix_amp_mod_table[0xFFu - 51u] &&
          lfo->amp_mod_register != 0u,
          "the resting bias pushes the register off its neutral entry");

    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 127u);
    check(state->mod_eg_bias_inv == 26u,
          "the EG-bias total is the range's value minus the routed sum");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amp_mod_register == slopsix_amp_mod_table[0xFFu - 26u] &&
          lfo->amp_mod_register < slopsix_amp_mod_table[0xFFu - 51u],
          "pushing the biased controller lowers the register - louder");

    /* The range-check pair: a full depth term gets trimmed to the
     * non-inverted total when it pushes past the byte. */
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 0u);
    lfo->amp_mod_depth = 0xFFu;
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amp_mod_register == slopsix_amp_mod_table[1u] &&
          lfo->amp_mod_register == 0xFFu,
          "the range-check pair trims the factor past the byte's range");

    /* The range total's own saturation: the range total clamps at $FF,
     * its complement is 0, and the resting total is the byte's top (the
     * firmware's own arithmetic, byte-verified at 0xE208/0xE222). */
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 99u);
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_BREATH, SLOPSIX_ASSIGN_EG);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_BREATH, 60u);
    check(state->mod_eg_bias_inv == 0xFFu,
          "the range total's saturation leaves the complement at $FF");
    lfo->phase = 0x8000u;
    slopsix_lfo_tick(state);
    check(lfo->amp_mod_register == slopsix_amp_mod_table[0u] &&
          lfo->amp_mod_register == 0xFFu,
          "the saturated range total rests the register on the table's top");

    /* The multi-source sums, with the range total inside the byte: the
     * amp factor sums the amp-routed sources, the EG-bias total is the
     * range total minus the EG-routed inputs' sum, and the pitch factor
     * sums the pitch-routed sources. */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_MOD_WHEEL,
                            SLOPSIX_ASSIGN_EG);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_MOD_WHEEL, 20u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 100u);   /* 19 */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_BREATH,
                            SLOPSIX_ASSIGN_AMP | SLOPSIX_ASSIGN_PITCH);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_BREATH, 99u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_BREATH, 64u);       /* 63 */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_FOOT,
                            SLOPSIX_ASSIGN_AMP);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_FOOT, 33u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_FOOT, 127u);        /* 42 */
    slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_AFTERTOUCH,
                            SLOPSIX_ASSIGN_EG);
    slopsix_set_ctrl_range(ctx, SLOPSIX_CTRL_AFTERTOUCH, 30u);
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_AFTERTOUCH, 127u);  /* 38 */
    {
        unsigned scaled_wheel = ((660u * 20u) >> 8) * 100u >> 8;  /* 19 */
        unsigned scaled_breath = (255u * 64u) >> 8;               /* 63 */
        unsigned scaled_foot = ((660u * 33u) >> 8) * 127u >> 8;   /* 42 */
        unsigned scaled_at = ((660u * 30u) >> 8) * 127u >> 8;     /* 38 */

        check((660u * 20u) >> 8 == 51u && scaled_wheel == 19u &&
              (660u * 33u) >> 8 == 85u && scaled_foot == 42u &&
              (660u * 30u) >> 8 == 77u && scaled_at == 38u &&
              scaled_breath == 63u,
              "the stage's own scaled inputs sit where they should");
        check(state->ctrl_scaled_input[SLOPSIX_CTRL_MOD_WHEEL] == 19u &&
              state->ctrl_scaled_input[SLOPSIX_CTRL_BREATH] == 63u &&
              state->ctrl_scaled_input[SLOPSIX_CTRL_FOOT] == 42u &&
              state->ctrl_scaled_input[SLOPSIX_CTRL_AFTERTOUCH] == 38u,
              "each source keeps its own scaled input");
        check(state->mod_amp_factor == scaled_breath + scaled_foot,
              "the amp factor sums the amp-routed sources");
        check(state->mod_eg_bias_inv == (51u + 77u) - (scaled_wheel +
                                                       scaled_at),
              "the EG-bias total is the range total minus the EG-routed sum");
    }

    /* The pitch composition: the pitch-routed sources' scaled inputs
     * join the LFO's depth term before the byte's clamp, and the term is
     * that factor times the sensitivity-scaled amplitude's high byte,
     * right by one. */
    pitch_depth = 100u;
    lfo->pitch_mod_depth = (uint8_t)pitch_depth;
    lfo->pitch_mod_sens = 255u;
    lfo->phase = 0x0000u;         /* the square's positive half */
    slopsix_lfo_tick(state);
    {
        /* The breath's 63 is the only pitch-routed source. */
        unsigned base = ((pitch_depth * 0xFFu) >> 8) + 63u;
        unsigned factor = base > 0xFFu ? 0xFFu : base;
        int32_t product = (int32_t)factor *
                          (int32_t)((255u * 127u) >> 8);

        check(state->lfo.pitch_mod_term ==
                  (int16_t)slopsix_floor_div(product, 1u),
              "the pitch factor carries the pitch-routed source");
        check(state->lfo.pitch_mod_word == state->lfo.pitch_mod_term,
              "the recomposed word is the recomposed term");
    }

    /* The setter recomposes the word immediately, without a tick: the
     * term follows the last tick's amplitude. */
    slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_BREATH, 0u);
    check(state->lfo.pitch_mod_word ==
              (int16_t)slopsix_floor_div((int32_t)((pitch_depth * 0xFFu) >>
                                                   8) *
                                             (int32_t)((255u * 127u) >> 8),
                                         1u),
          "a controller change recomposes the pitch word at once");

    /* The bend setter's path still recomposes beside the term. */
    slopsix_set_pitch_bend(ctx, 0x2000u);
    check(state->lfo.pitch_mod_word ==
              (int16_t)slopsix_floor_div((int32_t)((pitch_depth * 0xFFu) >>
                                                   8) *
                                             (int32_t)((255u * 127u) >> 8),
                                         1u),
          "the centred bend leaves the recomposed word unchanged");

    /* Reset returns the controller arithmetic to its zeros. */
    slopsix_reset(ctx);
    check(state->mod_amp_factor == 0u && state->mod_eg_bias_inv == 0u,
          "the reset clears the controller arithmetic");

    slopsix_destroy_context(ctx);
}

/* ------------------------------------------- Phase 5b: the public API */

/*
 * The API shape: the engine selection, the size-parameterised patch I/O,
 * the setters' error reporting and the capability queries.
 */
static void stage_api(void)
{
    slopsix_context ctx;
    uint8_t dump[SLOPSIX_VOICE_DUMP_SIZE];
    uint8_t dump2[SLOPSIX_VOICE_DUMP_SIZE];
    int16_t buffer[16];
    size_t i;

    printf("stage api\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, 44100u);
    check(ctx != NULL, "create_context returns a DX7 context");
    if (ctx == NULL) {
        return;
    }

    /* Version reporting. */
    check(slopsix_version_number() == SLOPSIX_VERSION_NUMBER,
          "slopsix_version_number() returns the header version");

    /* The capability queries. */
    check(slopsix_context_engine(ctx) == SLOPSIX_ENGINE_DX7,
          "the context reports the engine it was created for");
    check(slopsix_voice_count(ctx) == 16u,
          "the DX7 reports its sixteen voices");
    check(slopsix_operator_count(ctx) == 6u,
          "the DX7 reports its six operators");
    check(slopsix_algorithm_count(ctx) == 32u,
          "the DX7 reports its 32 algorithms");
    check(slopsix_voice_dump_size(ctx) == 156u,
          "the DX7 reports its 156-byte dump");
    check(slopsix_context_sample_rate(ctx) == 44100u,
          "the context reports its sample rate");
    check(slopsix_context_engine(NULL) == SLOPSIX_ENGINE_COUNT &&
          slopsix_voice_count(NULL) == 0u &&
          slopsix_operator_count(NULL) == 0u &&
          slopsix_algorithm_count(NULL) == 0u &&
          slopsix_voice_dump_size(NULL) == 0u &&
          slopsix_context_sample_rate(NULL) == 0u,
          "a null context answers zero");

    /* The engine selection: an engine that is not in the build is
     * rejected at creation, not at render time. */
    {
        slopsix_context dx7s =
            slopsix_create_context(SLOPSIX_ENGINE_DX7S, 0);
        slopsix_context unknown =
            slopsix_create_context((slopsix_engine)99, 0);

#if SLOPSIX_HAS_DX7S
        check(dx7s != NULL,
              "a DX7S context is created in a build with the personality");
        if (dx7s != NULL) {
            check(slopsix_context_engine(dx7s) == SLOPSIX_ENGINE_DX7S,
                  "the DX7S context reports its engine");
            check(slopsix_voice_count(dx7s) == 16u &&
                  slopsix_operator_count(dx7s) == 6u &&
                  slopsix_algorithm_count(dx7s) == 32u &&
                  slopsix_voice_dump_size(dx7s) == 156u,
                  "the DX7S reports the Mk1 shape (16 voices, 6 operators, "
                  "32 algorithms, 156-byte dump)");
            slopsix_destroy_context(dx7s);
        }
#else
        check(dx7s == NULL,
              "a DX7S context is rejected when the personality is absent");
#endif
        check(unknown == NULL, "an unknown engine is rejected at creation");
    }

    /* The patch I/O is size-parameterised and validates the size. */
    memset(dump, 0, sizeof(dump));
    dump[SLOPSIX_DUMP_ALGORITHM] = 1u;
    check(slopsix_set_voice(ctx, dump, sizeof(dump)) == SLOPSIX_OK,
          "a dump of the engine's own size loads");
    check(slopsix_set_voice(ctx, dump, sizeof(dump) - 1u) ==
          SLOPSIX_ERR_RANGE,
          "a dump of the wrong size is rejected");
    check(slopsix_get_voice(ctx, dump2, sizeof(dump2)) == SLOPSIX_OK &&
          memcmp(dump, dump2, sizeof(dump)) == 0,
          "the dump round-trips");
    check(slopsix_get_voice(ctx, dump2, sizeof(dump2) + 1u) ==
          SLOPSIX_ERR_RANGE,
          "a read of the wrong size is rejected");
    check(slopsix_set_voice(ctx, NULL, sizeof(dump)) == SLOPSIX_ERR_INVALID,
          "a null dump is rejected");
    check(slopsix_set_voice(NULL, dump, sizeof(dump)) == SLOPSIX_ERR_INVALID,
          "a null context is rejected");
    check(slopsix_get_voice(NULL, dump2, sizeof(dump2)) ==
          SLOPSIX_ERR_INVALID,
          "a null context is rejected on the read");

    /* The setters report what they did. */
    check(slopsix_set_pitch_bend(ctx, 0x4000u) == SLOPSIX_ERR_RANGE,
          "a bend above 14 bits is reported");
    check(SLOPSIX_CONTEXT(ctx)->pitch_bend == 0x3FFFu,
          "the bend is clamped to 14 bits");
    check(slopsix_set_pitch_bend(ctx, 0x2000u) == SLOPSIX_OK,
          "a bend inside the range is accepted");
    check(slopsix_set_pitch_bend_range(ctx, 13u, 0u) == SLOPSIX_ERR_RANGE,
          "a bend range above twelve semitones is reported");
    check(SLOPSIX_CONTEXT(ctx)->bend_range == 0u,
          "a bend range above twelve takes the hardware's validation form");
    check(slopsix_set_pitch_bend_range(ctx, 2u, 13u) == SLOPSIX_OK &&
          SLOPSIX_CONTEXT(ctx)->bend_step == 13u,
          "a bend step in the register's 4-bit domain is stored as it is");
    check(slopsix_set_pitch_bend_range(ctx, 2u, 16u) == SLOPSIX_ERR_RANGE &&
          SLOPSIX_CONTEXT(ctx)->bend_step == 0u,
          "a bend step outside the 4-bit domain takes the validation form");
    check(slopsix_set_ctrl_value(ctx, SLOPSIX_CTRL_MOD_WHEEL, 200u) ==
          SLOPSIX_ERR_RANGE,
          "a controller value above 127 is reported");
    check(slopsix_set_ctrl_value(ctx, (slopsix_ctrl_source)9, 0u) ==
          SLOPSIX_ERR_INVALID,
          "an unknown controller source is invalid");
    check(slopsix_set_ctrl_assign(ctx, SLOPSIX_CTRL_FOOT, 0x8u) ==
          SLOPSIX_ERR_RANGE,
          "unknown routing bits are reported");
    check(slopsix_set_midi_volume(ctx, 9u) == SLOPSIX_ERR_RANGE,
          "a master volume above seven is reported");
    check(SLOPSIX_CONTEXT(ctx)->master_volume == 7u,
          "the master volume is clamped to seven");
    check(slopsix_note_on(ctx, 60u, 200u) == SLOPSIX_ERR_RANGE,
          "a velocity above 127 is reported");
    check(slopsix_note_off(ctx, 128u) == SLOPSIX_ERR_RANGE,
          "a note off above 127 is reported");
    check(slopsix_note_on(ctx, 60u, 100u) == SLOPSIX_OK,
          "a note event inside the range is accepted");
    check(slopsix_set_midi_volume(NULL, 3u) == SLOPSIX_ERR_INVALID,
          "a null context is rejected by the setters");
    check(slopsix_set_output_gain(ctx, SLOPSIX_Q16_ONE + 1) ==
          SLOPSIX_ERR_RANGE,
          "an output volume above unity is reported");
    check(SLOPSIX_CONTEXT(ctx)->output_volume_q16 == SLOPSIX_Q16_ONE,
          "the output volume is clamped to unity");
    check(slopsix_set_output_gain(ctx, -1) == SLOPSIX_ERR_RANGE,
          "a negative output volume is reported");
    check(SLOPSIX_CONTEXT(ctx)->output_volume_q16 == 0,
          "a negative output volume clamps to silence");
    check(slopsix_set_output_gain(NULL, 0) == SLOPSIX_ERR_INVALID,
          "a null context is rejected by the output volume setter");

    /* The pan is an extra: without the extras build the call says so. */
#if SLOPSIX_HAS_EXTRAS
    check(slopsix_set_pan(ctx, 0) == SLOPSIX_OK,
          "the extras build accepts the pan");
    check(slopsix_set_pan(ctx, SLOPSIX_Q16_ONE + 1) == SLOPSIX_ERR_RANGE,
          "a pan beyond hard right is reported");
    check(SLOPSIX_CONTEXT(ctx)->pan_q16 == SLOPSIX_Q16_ONE,
          "the pan is clamped to hard right");
#else
    check(slopsix_set_pan(ctx, 0) == SLOPSIX_ERR_UNSUPPORTED,
          "the pan is unsupported without the extras build");
    check(SLOPSIX_CONTEXT(ctx)->pan_q16 == SLOPSIX_Q16_ONE / 2,
          "the output stays at centre without the extras build");
#endif

    /* The render path still runs through the resolved entry point. */
    for (i = 0; i < sizeof(buffer) / sizeof(buffer[0]); i++) {
        buffer[i] = 1234;
    }
    slopsix_process(ctx, buffer, 8u);
    check(buffer[0] != 1234 || buffer[1] != 1234,
          "the render call writes the buffer");

    /* The reset returns the context to its defaults. */
    slopsix_set_midi_volume(ctx, 3u);
    slopsix_set_output_gain(ctx, 0);
    slopsix_set_pitch_bend(ctx, 0x3000u);
    slopsix_reset(ctx);
    check(SLOPSIX_CONTEXT(ctx)->master_volume == 7u,
          "the reset restores the master volume");
    check(SLOPSIX_CONTEXT(ctx)->output_volume_q16 == SLOPSIX_Q16_ONE,
          "the reset restores the output volume");
    check(SLOPSIX_CONTEXT(ctx)->pitch_bend == 0x2000u,
          "the reset restores the bend");

    slopsix_destroy_context(ctx);
}

/* ------------------------------------------------------------ extras: the pan */

static void stage_extras(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    const int32_t centre = SLOPSIX_Q16_ONE / 2;

    printf("stage extras\n");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    check(state->pan_q16 == centre, "the default pan is centre");

#if SLOPSIX_HAS_EXTRAS
    slopsix_set_pan(ctx, 0);
    check(state->pan_q16 == 0, "the extras build stores the pan");
    slopsix_set_pan(ctx, SLOPSIX_Q16_ONE + 1);
    check(state->pan_q16 == SLOPSIX_Q16_ONE,
          "the extras build clamps the pan to hard right");
    slopsix_set_pan(ctx, -1);
    check(state->pan_q16 == 0, "the extras build clamps the pan to hard left");
#else
    slopsix_set_pan(ctx, 0);
    check(state->pan_q16 == centre,
          "without the extras build set_pan leaves the output at centre");
#endif

    slopsix_destroy_context(ctx);
}


static int32_t porta_note_word(uint8_t note)
{
    return (int32_t)(slopsix_note_word[note] & ~3u);
}

static void stage_porta(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    int32_t current;
    int32_t target;
    int32_t gliss;
    int32_t out;
    int32_t w60;
    int32_t w64;
    int32_t w72;
    uint16_t changed;

    printf("stage porta\n");

    /* The rate law: time 0 is the table's last entry (0xFF, instant),
     * time 99 its first (1).  The table is the pitch EG's own. */
    check(slopsix_pitch_eg_rate_value(99u) == 0xFFu,
          "portamento time 0 reads the instantaneous rate");
    check(slopsix_pitch_eg_rate_value(0u) == 1u,
          "portamento time 99 reads the slowest rate");

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    if (ctx == NULL) {
        check(0, "create_context returns a context");
        return;
    }
    state = SLOPSIX_CONTEXT(ctx);

    check(state->porta_time == 0u && state->porta_rate == 0xFFu &&
          !state->porta_full_time && !state->porta_pedal &&
          !state->glissando,
          "portamento resets to the hardware's instant, fingered state");
    check(state->voices[0].porta_current == SLOPSIX_PORTA_RESET_WORD &&
          state->porta_last_target == SLOPSIX_PORTA_RESET_WORD,
          "the pitch buffers reset to the firmware's C2 word");

    check(slopsix_set_portamento_time(ctx, 99u) == SLOPSIX_OK &&
          state->porta_rate == 1u,
          "the time setter caches the rate table entry");
    check(slopsix_set_portamento_time(ctx, 120u) == SLOPSIX_ERR_RANGE &&
          state->porta_time == 99u,
          "a time past 99 clamps and reports a range error");
    check(slopsix_set_portamento_mode(ctx,
              (slopsix_portamento_mode)2) == SLOPSIX_ERR_INVALID &&
          !state->porta_full_time,
          "an unknown portamento mode is rejected");
    check(slopsix_set_portamento_mode(ctx, SLOPSIX_PORTA_FULL_TIME) ==
              SLOPSIX_OK && state->porta_full_time,
          "the mode setter stores full-time mode");
    slopsix_destroy_context(ctx);

    /* The proportional step: the distance's high byte is 0x40 at four
     * octaves, so ((0x40 >> 2) + 1) * rate = 17 * 4. */
    current = 0;
    target = 0x4000;
    gliss = 0;
    out = slopsix_porta_step_word(&current, target, &gliss, 4u, false);
    check(current == 68 && out == 68,
          "the portamento step follows the distance's high byte");
    current = target - 1;
    out = slopsix_porta_step_word(&current, target, &gliss, 0xFFu, false);
    check(current == target && out == target,
          "an overshooting step clamps at the target");
    current = 0x4000;
    target = 0;
    out = slopsix_porta_step_word(&current, target, &gliss, 4u, false);
    check(current == 0x4000 - 68 && out == current,
          "the downward step uses the same proportional law");

    /* Glissando walks the note table's semitone stair: 0x154, 0x2A8,
     * 0x400 (the three-step cycle's 344-unit entry), then 0x554. */
    current = 0;
    gliss = 0;
    target = 0x400;
    out = slopsix_porta_step_word(&current, target, &gliss, 0xFFu, true);
    check(out == 0x154 && gliss == 0x154,
          "glissando's first step is one semitone");
    out = slopsix_porta_step_word(&current, target, &gliss, 0xFFu, true);
    check(out == 0x2A8, "glissando's second step is one semitone");
    out = slopsix_porta_step_word(&current, target, &gliss, 0xFFu, true);
    check(out == 0x400, "glissando crosses the cycle's 344-unit boundary");
    out = slopsix_porta_step_word(&current, target, &gliss, 0xFFu, true);
    check(out == 0x400, "glissando holds once it reaches the target");
    check(slopsix_porta_step_word(&current, target, &gliss, 0xFFu, false) ==
              target,
          "glissando off leaves the continuous word audible");

    /* The half-bank toggle: bank 0 advances voices 0..7 only, bank 1
     * advances 8..15 only, and the mask names the voices that moved. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    state->porta_rate = 4u;
    state->voices[0].state = SLOPSIX_VOICE_ON;
    state->voices[0].porta_current = 0;
    state->voices[0].porta_target = 0x4000;
    state->voices[8].state = SLOPSIX_VOICE_ON;
    state->voices[8].porta_current = 0;
    state->voices[8].porta_target = 0x4000;
    changed = slopsix_porta_process(state, 0u);
    check(changed == 0x0001u && state->voices[0].porta_current == 68 &&
          state->voices[8].porta_current == 0,
          "PORTA_PROCESS bank 0 moves voices 0..7 only");
    changed = slopsix_porta_process(state, 1u);
    check(changed == 0x0100u && state->voices[8].porta_current == 68 &&
          state->voices[0].porta_current == 68,
          "PORTA_PROCESS bank 1 moves voices 8..15 only");
    slopsix_destroy_context(ctx);

    w60 = porta_note_word(60u);
    w64 = porta_note_word(64u);
    w72 = porta_note_word(72u);

    /* Polyphonic retain mode: the new voice starts from the last note's
     * target, and the older voice keeps its own target. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    check(slopsix_set_portamento_time(ctx, 50u) == SLOPSIX_OK,
          "poly: set a slow portamento time");
    check(slopsix_set_portamento_pedal(ctx, true) == SLOPSIX_OK,
          "poly: press the portamento pedal");
    slopsix_note_on(ctx, 60u, 100u);
    slopsix_note_on(ctx, 72u, 100u);
    check(state->voices[1].porta_current == w60 &&
          state->voices[1].porta_target == w72 &&
          state->voices[0].porta_target == w60,
          "poly retain starts the new voice at the last note's target");
    changed = slopsix_porta_process(state, 0u);
    check((changed & 0x0002u) != 0u &&
          state->voices[1].porta_current > w60 &&
          state->voices[1].porta_current < w72,
          "poly retain glides the new voice");

    /* The render loop drives the same step on the OCF ticks and rewrites
     * the affected voice's phase increment. */
    {
        int16_t buffer[2 * 1024];
        int32_t offset;
        int32_t voice;
        int32_t op_word;
        uint32_t expected;

        slopsix_process(ctx, buffer, 1024u);
        check(state->voices[1].porta_current > w60,
              "the render call advances the polyphonic glide");
        offset = slopsix_mod_pitch_offset(state) +
                 slopsix_pitch_eg_offset(&state->engines[1].pitch_eg);
        voice = slopsix_voice_word_from_base(
            state->voices[1].porta_current & ~3,
            state->master_tune_q16, offset);
        op_word = slopsix_op_freq_from_voice(
            &state->engines[1].params[0], voice);
        expected = slopsix_increment_from_word(op_word,
            state->sample_rate);
        check(state->engines[1].ops[0].increment == expected,
              "the glide's word reaches the phase increment");
    }
    slopsix_destroy_context(ctx);

    /* Turning glissando off switches the audible word from the stair to
     * the continuous portamento buffer.  A parked buffer must re-pitch a
     * sounding voice immediately, not leave it on the stale stair. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_portamento_time(ctx, 50u);
    slopsix_set_portamento_pedal(ctx, true);
    slopsix_set_glissando(ctx, true);
    slopsix_note_on(ctx, 60u, 100u);
    slopsix_note_on(ctx, 72u, 100u);
    state->voices[1].porta_current = w72;
    state->voices[1].gliss_current = w64;
    slopsix_voice_engine_update_frequency(state, &state->voices[1],
        &state->engines[1]);
    slopsix_set_glissando(ctx, false);
    {
        int32_t offset = slopsix_mod_pitch_offset(state) +
            slopsix_pitch_eg_offset(&state->engines[1].pitch_eg);
        int32_t voice = slopsix_voice_word_from_base(
            state->voices[1].porta_current & ~3,
            state->master_tune_q16, offset);
        int32_t op_word = slopsix_op_freq_from_voice(
            &state->engines[1].params[0], voice);

        check(state->engines[1].ops[0].increment ==
                  slopsix_increment_from_word(op_word,
                      state->sample_rate),
              "turning glissando off re-pitches the sounding voice");
    }
    slopsix_destroy_context(ctx);

    /* Polyphonic follow mode retargets the pedal-held voice. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_portamento_time(ctx, 50u);
    slopsix_set_portamento_mode(ctx, SLOPSIX_PORTA_FULL_TIME);
    slopsix_set_portamento_pedal(ctx, true);
    slopsix_note_on(ctx, 60u, 100u);
    slopsix_set_sustain(ctx, true);
    slopsix_note_off(ctx, 60u);
    slopsix_note_on(ctx, 72u, 100u);
    check(state->voices[0].state == SLOPSIX_VOICE_ON &&
          !state->voices[0].held &&
          state->voices[0].porta_target == w72,
          "poly follow retargets the pedal-held voice");
    check(state->voices[1].porta_current == w60 &&
          state->voices[1].porta_target == w72,
          "poly follow also starts the new voice at the last target");
    slopsix_destroy_context(ctx);

    /* Mono fingered mode glides on legato without the pedal. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_mono_mode(ctx, true);
    slopsix_set_portamento_time(ctx, 50u);
    slopsix_note_on(ctx, 60u, 100u);
    check(state->voices[0].porta_current == w60 &&
          state->voices[0].porta_target == w60,
          "mono's first key snaps to its target");
    slopsix_note_on(ctx, 64u, 100u);
    check(state->voices[0].porta_current == w60 &&
          state->voices[0].porta_target == w64,
          "mono fingered legato keeps the buffer and moves the target");
    (void)slopsix_porta_process(state, 0u);
    check(state->voices[0].porta_current > w60,
          "the mono glide steps toward the new key");
    slopsix_note_off(ctx, 64u);
    check(state->voices[0].porta_target == w60 &&
          state->voices[0].porta_current > w60,
          "mono fingered fallback glides back to the remaining key");
    slopsix_destroy_context(ctx);

    /* Mono full-time needs the pedal. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_mono_mode(ctx, true);
    slopsix_set_portamento_time(ctx, 50u);
    slopsix_set_portamento_mode(ctx, SLOPSIX_PORTA_FULL_TIME);
    slopsix_note_on(ctx, 60u, 100u);
    slopsix_note_on(ctx, 64u, 100u);
    check(state->voices[0].porta_current == w64 &&
          state->voices[0].porta_target == w64,
          "mono full-time without the pedal jumps to the new key");
    slopsix_destroy_context(ctx);

    /* Mono full-time with the pedal keeps the glide, including the
     * first key's start from the reset buffers. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_mono_mode(ctx, true);
    slopsix_set_portamento_time(ctx, 50u);
    slopsix_set_portamento_mode(ctx, SLOPSIX_PORTA_FULL_TIME);
    slopsix_set_portamento_pedal(ctx, true);
    slopsix_note_on(ctx, 60u, 100u);
    check(state->voices[0].porta_current == SLOPSIX_PORTA_RESET_WORD &&
          state->voices[0].porta_target == w60,
          "mono full-time with the pedal keeps the first key's glide");
    slopsix_note_on(ctx, 64u, 100u);
    check(state->voices[0].porta_current == SLOPSIX_PORTA_RESET_WORD &&
          state->voices[0].porta_target == w64,
          "mono full-time with the pedal glides on legato");
    slopsix_destroy_context(ctx);

    /* Without the pedal, a polyphonic note-on is instantaneous no matter
     * how slow the time is. */
    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7,
        SLOPSIX_DEFAULT_SAMPLE_RATE);
    state = SLOPSIX_CONTEXT(ctx);
    slopsix_set_portamento_time(ctx, 99u);
    slopsix_note_on(ctx, 60u, 100u);
    slopsix_note_on(ctx, 72u, 100u);
    check(state->voices[1].porta_current == w72 &&
          state->voices[1].porta_target == w72,
          "poly portamento is gated by the pedal");
    slopsix_destroy_context(ctx);
}

int main(int argc, char **argv)
{
    int i;
    int ran = 0;

#if defined(SLOPSIX_DX7S)
#if SLOPSIX_HAS_DX7S
    printf("configuration: DX7S personality enabled\n");
#else
    printf("configuration: DX7S configuration (personality units absent)\n");
#endif
#else
    printf("configuration: DX7 only\n");
#endif
#if SLOPSIX_HAS_EXTRAS
    printf("configuration: extras enabled (pan is available)\n");
#else
    printf("configuration: extras disabled (mono, pan is a no-op)\n");
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--init") == 0) {
            stage_init();
            ran = 1;
        } else if (strcmp(argv[i], "--silence") == 0) {
            stage_silence();
            ran = 1;
        } else if (strcmp(argv[i], "--tables") == 0) {
            stage_tables();
            ran = 1;
        } else if (strcmp(argv[i], "--voices") == 0) {
            stage_voices();
            ran = 1;
        } else if (strcmp(argv[i], "--monovoice") == 0) {
            stage_monovoice();
            ran = 1;
        } else if (strcmp(argv[i], "--operator") == 0) {
            stage_operator();
            ran = 1;
        } else if (strcmp(argv[i], "--freq") == 0) {
            stage_freq();
            ran = 1;
        } else if (strcmp(argv[i], "--algorithms") == 0) {
            stage_algorithms();
            ran = 1;
        } else if (strcmp(argv[i], "--graph") == 0) {
            stage_graph();
            ran = 1;
        } else if (strcmp(argv[i], "--eg") == 0) {
            stage_eg();
            ran = 1;
        } else if (strcmp(argv[i], "--level") == 0) {
            stage_level();
            ran = 1;
        } else if (strcmp(argv[i], "--com") == 0) {
            stage_com();
            ran = 1;
        } else if (strcmp(argv[i], "--bend") == 0) {
            stage_bend();
            ran = 1;
        } else if (strcmp(argv[i], "--pitcheg") == 0) {
            stage_pitcheg();
            ran = 1;
        } else if (strcmp(argv[i], "--porta") == 0) {
            stage_porta();
            ran = 1;
        } else if (strcmp(argv[i], "--lfo") == 0) {
            stage_lfo();
            ran = 1;
        } else if (strcmp(argv[i], "--plfo") == 0) {
            stage_plfo();
            ran = 1;
        } else if (strcmp(argv[i], "--pegr") == 0) {
            stage_peg_range();
            ran = 1;
        } else if (strcmp(argv[i], "--unison") == 0) {
            stage_unison();
            ran = 1;
        } else if (strcmp(argv[i], "--params") == 0) {
            stage_personality_params();
            ran = 1;
        } else if (strcmp(argv[i], "--ctrl") == 0) {
            stage_ctrl();
            ran = 1;
        } else if (strcmp(argv[i], "--filter") == 0) {
            stage_filter();
            ran = 1;
        } else if (strcmp(argv[i], "--output") == 0) {
            stage_output();
            ran = 1;
        } else if (strcmp(argv[i], "--api") == 0) {
            stage_api();
            ran = 1;
        } else if (strcmp(argv[i], "--extras") == 0) {
            stage_extras();
            ran = 1;
        } else if (strcmp(argv[i], "--render") == 0) {
            stage_render();
            ran = 1;
        } else if (strcmp(argv[i], "--blockgraph") == 0) {
            stage_blockgraph();
            ran = 1;
        } else if (strcmp(argv[i], "--blockprocess") == 0) {
            stage_blockprocess();
            ran = 1;
        } else {
            fprintf(stderr, "unknown stage: %s\n", argv[i]);
            return 2;
        }
    }

    if (!ran) {
        stage_init();
        stage_silence();
        stage_tables();
        stage_voices();
        stage_monovoice();
        stage_operator();
        stage_freq();
        stage_algorithms();
        stage_graph();
        stage_eg();
        stage_level();
        stage_com();
        stage_bend();
        stage_pitcheg();
        stage_porta();
        stage_lfo();
        stage_plfo();
        stage_peg_range();
        stage_unison();
        stage_personality_params();
        stage_ctrl();
        stage_filter();
        stage_output();
        stage_api();
        stage_extras();
        stage_render();
        stage_blockgraph();
        stage_blockprocess();
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
