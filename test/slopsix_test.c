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
 *   voices   voice state transitions and sustain pedal logic
 *
 * The harness is a host tool, not the library: it may use whatever it
 * likes, but it links only the public library plus the internal table
 * declarations it needs to check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slopsix_internal.h"

extern const int16_t slopsix_logsin_quarter[1024];
extern const uint16_t slopsix_exp_q11[1024];
extern const uint8_t slopsix_note_t[128];
extern const uint16_t slopsix_note_word[128];
extern const uint8_t slopsix_com_log[6];

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
    check(slopsix_com_log[4] == 35, "com[4] == 35");
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

int main(int argc, char **argv)
{
    int i;
    int ran = 0;

#if defined(SLOPSIX_DX100)
    printf("configuration: DX100 personality enabled\n");
#else
    printf("configuration: DX7 only\n");
#endif
#if defined(SLOPSIX_EXTRAS)
    printf("configuration: extensions enabled\n");
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
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
