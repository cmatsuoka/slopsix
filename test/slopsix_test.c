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
 *   voices      voice state transitions and sustain pedal logic
 *   operator    the operator core against the closed sine recipe
 *   freq        the frequency path: words, grid and pitch
 *   algorithms  the 32 algorithms' decoded connections
 *   graph       the operator-graph walk
 *   bend        the pitch bend: the curve, its quantisation and its scale
 *   pitcheg     the pitch envelope: its tables, stage walk and cadence
 *   output      the output stages: compander, DAC step, pan law and master
 *   extras      the pan is an extra: available or a no-op by build
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

    ctx = slopsix_create_context(0);
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

    ctx = slopsix_create_context(SLOPSIX_DEFAULT_SAMPLE_RATE);
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
    check(slopsix_note_t[0] == 0, "note_t[0] == 0 (key-code-0 default)");
    check(slopsix_note_t[1] == 0, "note_t[1] == 0");
    check(slopsix_note_t[13] == 16, "note_t[13] == 16");
    check(slopsix_note_t[127] == 168, "note_t[127] == 168");
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

    ctx = slopsix_create_context(SLOPSIX_DEFAULT_SAMPLE_RATE);
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
                                    slopsix_algorithm_get(0), &plain_out);
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
                                        slopsix_algorithm_get(0), &out);
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
                                        slopsix_algorithm_get(6), &out);
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
         * operator, except for its own feedback path. */
        for (op = 1; op <= SLOPSIX_OP_COUNT; op++) {
            if (decoded->ops[op - 1u].source == SLOPSIX_MOD_OPERATOR &&
                decoded->ops[op - 1u].source_op <= (uint8_t)op) {
                order_ok = 0;
            }
        }
    }

    check(carriers_ok, "all 32 algorithms decode the documented carriers");
    check(feedback_ok,
          "all 32 algorithms decode the documented feedback operator");
    check(order_ok,
          "no operator is modulated by an equal or lower-numbered one");
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

        for (i = 0; i < 4000; i++) {
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

    /* The amplitude modulation: it lowers the level of the sensitive
     * operators and never raises it. */
    check(slopsix_ams_reduction_q8(0, 512) == 0,
          "an operator without amplitude modulation is not affected");
    check(slopsix_ams_reduction_q8(3, 512) > 0,
          "the amplitude modulation lowers the level");
    check(slopsix_ams_reduction_q8(3, 0) == 0,
          "the amplitude modulation does nothing without an amount");
    check(slopsix_ams_reduction_q8(1, 512) < slopsix_ams_reduction_q8(3, 512),
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

        slopsix_voice_render_sample(&voice, slopsix_algorithm_get(31), &out);
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

        slopsix_voice_render_sample(&voice, slopsix_algorithm_get(15), &out);
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

    ctx = slopsix_create_context(SLOPSIX_DEFAULT_SAMPLE_RATE);
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

    /* The DAC step: the 12-bit grid at one eighth of the bus unit and
     * the rails' saturation. */
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
    check(slopsix_dac_step(16377) == 2047 && slopsix_dac_step(16384) == 2047,
          "the positive rail saturates at 2047");
    check(slopsix_dac_step(-16376) == -2047 &&
          slopsix_dac_step(-16377) == -2048 &&
          slopsix_dac_step(-16384) == -2048,
          "the negative rail saturates at -2048");
    check(slopsix_dac_step(262016) == 2047 && slopsix_dac_step(-262144) == -2048,
          "a full 16-voice bus saturates instead of wrapping");
    {
        int monotone = 1;

        for (i = 0; i < 80000u; i++) {
            int32_t bus = (int32_t)i - 40000;
            int32_t code = slopsix_dac_step(bus);
            int32_t lower = slopsix_dac_step(bus - 1);

            if (code < lower) {
                monotone = 0;
            }
            if (code > 2047 || code < -2048) {
                monotone = 0;
            }
        }
        check(monotone == 1, "the DAC step is monotone and stays in the window");
    }

    /* The master ladder: the resistor string's taps as exact Q16 gains. */
    for (i = 0; i < 8u; i++) {
        if (slopsix_master_gain_q16((unsigned)i) != ladder[i]) {
            break;
        }
    }
    check(i == 8u, "the ladder's eight taps match the resistor string");
    check(slopsix_master_gain_q16(8u) == ladder[7],
          "a level past the top of the ladder clamps to it");

    /* The final conversion: rounding to the int16 grid at unity gain. */
    check(slopsix_output_scale(2047, SLOPSIX_Q16_ONE) == 32752,
          "the code window's top lands on 32752 at unity gain");
    check(slopsix_output_scale(-2048, SLOPSIX_Q16_ONE) == -32768,
          "the code window's floor lands on -32768");
    check(slopsix_output_scale(2046, SLOPSIX_Q16_ONE) == 32736,
          "unity gain keeps the int16 output on the 12-bit grid");
    check(slopsix_output_scale(100, SLOPSIX_Q16_ONE) == 1600,
          "unity gain keeps the int16 output on the 12-bit grid");
    check(slopsix_output_scale(2047, 36052) == 18017,
          "a ladder tap scales the code");
    check(slopsix_output_scale(-2048, 1340) == -670,
          "the quietest tap scales the code");

    {
        slopsix_context ctx;
        slopsix_context_state *state;
        int16_t l1, r1, l2, r2;

        ctx = slopsix_create_context(SLOPSIX_DEFAULT_SAMPLE_RATE);
        if (ctx == NULL) {
            check(0, "create_context returns a context");
            return;
        }
        state = SLOPSIX_CONTEXT(ctx);

        /* The default context: the hardware's reset volume. */
        check(state->master_volume == 7u,
              "the default master volume is the hardware's reset value");

        /* The composite at default settings: full scale in, one voice's
         * window top out, left and right identical. */
        slopsix_output_mix(state, 16376, &l1, &r1);
        check(l1 == 32752 && r1 == 32752,
              "the mixed bus's full scale lands on the code window's top");
        slopsix_output_mix(state, 16384, &l1, &r1);
        check(l1 == 32752 && r1 == 32752,
              "a bus past one voice's window saturates instead of growing");
        slopsix_output_mix(state, -16384, &l1, &r1);
        check(l1 == -32768 && r1 == -32768,
              "the negative rail saturates at -32768");

        /* Determinism: the same state and input produce the same output. */
        slopsix_output_mix(state, 12345, &l1, &r1);
        slopsix_output_mix(state, 12345, &l2, &r2);
        check(l1 == l2 && r1 == r2, "the mix is deterministic");

        /* The volume ladder through the composite. */
        slopsix_set_master_volume(ctx, 0u);
        slopsix_output_mix(state, 16376, &l1, &r1);
        check(l1 == 670 && r1 == 670,
              "volume 0 is the ladder's bottom tap, not silence");
        slopsix_set_master_volume(ctx, 3u);
        slopsix_output_mix(state, 16376, &l1, &r1);
        check(l1 == 3215 && r1 == 3215,
              "volume 3's tap scales the mixed code");
        slopsix_set_master_volume(ctx, 7u);

#if SLOPSIX_HAS_EXTRAS
        /* The pan law through the composite: centre is bit-identical
         * mono-in-both-channels, the extremes are hard splits.  The bus
         * 8000 maps to code 1000, which at unity gain is 16000. */
        slopsix_output_mix(state, -12800, &l1, &r1);
        check(l1 == -25600 && r1 == -25600,
              "the default centre keeps both channels identical");
        slopsix_set_pan(ctx, 0);
        slopsix_output_mix(state, 8000, &l1, &r1);
        check(l1 == 16000 && r1 == 0, "hard left carries the mono signal alone");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE);
        slopsix_output_mix(state, 8000, &l1, &r1);
        check(l1 == 0 && r1 == 16000, "hard right carries the mono signal alone");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE / 4);
        slopsix_output_mix(state, 8000, &l1, &r1);
        check(l1 == 16000 && r1 == 8000,
              "the left half keeps the mono level, the right fades");
        slopsix_set_pan(ctx, 3 * SLOPSIX_Q16_ONE / 4);
        slopsix_output_mix(state, 8000, &l1, &r1);
        check(l1 == 8000 && r1 == 16000,
              "the right half keeps the mono level, the left fades");
        slopsix_set_pan(ctx, SLOPSIX_Q16_ONE / 2);
        slopsix_output_mix(state, -12800, &l1, &r1);
        check(l1 == -25600 && r1 == -25600,
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
        slopsix_output_mix(state, 8000, &l1, &r1);
        check(l1 == 16000 && r1 == 16000,
              "without the extras both channels carry the mono signal");
#endif

        slopsix_destroy_context(ctx);
    }
}

/* --------------------------------------------------- extras: the pan */

static void stage_extras(void)
{
    slopsix_context ctx;
    slopsix_context_state *state;
    const int32_t centre = SLOPSIX_Q16_ONE / 2;

    printf("stage extras\n");

    ctx = slopsix_create_context(SLOPSIX_DEFAULT_SAMPLE_RATE);
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

int main(int argc, char **argv)
{
    int i;
    int ran = 0;

#if defined(SLOPSIX_DX100)
    printf("configuration: DX100 personality enabled\n");
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
        } else if (strcmp(argv[i], "--output") == 0) {
            stage_output();
            ran = 1;
        } else if (strcmp(argv[i], "--extras") == 0) {
            stage_extras();
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
        stage_operator();
        stage_freq();
        stage_algorithms();
        stage_graph();
        stage_eg();
        stage_level();
        stage_com();
        stage_bend();
        stage_pitcheg();
        stage_output();
        stage_extras();
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
