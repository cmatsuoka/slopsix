/*
 * play_keys.c - play the DX7 engine from the terminal keyboard.
 *
 * The example loads a small built-in voice and streams its stereo output
 * through OpenAL.  Audio is queued as four short buffers, which keeps the
 * added latency around 20 ms at 48 kHz while leaving headroom for ordinary
 * operating-system scheduling jitter.
 *
 * The terminal delivers key presses but no key releases, so a note key
 * toggles its note: press once to key on, press again to release it.
 * Pressing Space releases every key.  The octave keys transpose the
 * keyboard while later notes use the new octave.
 *
 * Keys:
 *   lower row   Z S X D C V G B H N J M ,   C .. C one octave up
 *   upper row   Q 2 W 3 E R 5 T 6 Y 7 U I   C one octave up and beyond
 *   [ ]         octave down / up
 *   Space       all notes off
 *   Q or Esc    quit  (Ctrl-C also works)
 *
 * The built-in patch is an electric piano built on the DX7's factory
 * "E.PIANO 1" voice, so the example shows a real FM patch rather than a
 * bare test tone.  A later version can add a raw or SYSEX voice loader.
 */

#include <AL/al.h>
#include <AL/alc.h>

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "slopsix.h"

#define VOICE_DUMP_SIZE 156u
#define DEFAULT_BASE_NOTE 48

#define BUFFER_FRAMES 256u
#define BUFFER_COUNT 4u
#define POLL_TIMEOUT_MS 2

static struct termios saved_termios;
static volatile sig_atomic_t terminal_is_raw = 0;

static void restore_terminal(void)
{
    if (terminal_is_raw) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        terminal_is_raw = 0;
    }
}

static void on_fatal_signal(int sig)
{
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_terminal_guards(void)
{
    static const int signals[] = {
        SIGINT, SIGTERM, SIGHUP, SIGQUIT,
        SIGABRT, SIGSEGV, SIGFPE, SIGBUS, SIGILL
    };
    size_t i;

    (void)atexit(restore_terminal);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        (void)signal(signals[i], on_fatal_signal);
    }
}

static int set_raw_mode(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "This example needs an interactive terminal.\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        perror("tcgetattr");
        return -1;
    }

    raw = saved_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        perror("tcsetattr");
        return -1;
    }
    terminal_is_raw = 1;
    return 0;
}

/* The DX7 voice dump: six 21-byte operator blocks, operator 6 first, then
 * the global parameters.  This one holds the factory "E.PIANO 1" voice
 * (algorithm 4, feedback 6). */
static const uint8_t default_voice[VOICE_DUMP_SIZE] = {
    /* OP6 */ 95, 29, 20, 50, 99, 95, 0, 0, 41, 0, 19, 0, 0, 3, 0, 6,
              79, 0, 1, 0, 14,
    /* OP5 */ 95, 20, 20, 50, 99, 95, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0,
              99, 0, 1, 0, 0,
    /* OP4 */ 95, 29, 20, 50, 99, 95, 0, 0, 0, 0, 0, 0, 0, 3, 0, 6,
              89, 0, 1, 0, 7,
    /* OP3 */ 95, 20, 20, 50, 99, 95, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2,
              99, 0, 1, 0, 7,
    /* OP2 */ 95, 50, 35, 78, 99, 75, 0, 0, 0, 0, 0, 0, 0, 3, 0, 7,
              58, 0, 14, 0, 7,
    /* OP1 */ 96, 25, 25, 67, 99, 75, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2,
              99, 0, 1, 0, 10,
    /* pitch EG rates and levels, algorithm 4, feedback 6, LFO, transpose,
     * then the ten name characters and the unused enable byte. */
              94, 67, 95, 60, 50, 50, 50, 50, 4, 6, 0, 34, 33, 0, 0, 0,
              4, 3, 24, 'E', '.', 'P', 'I', 'A', 'N', 'O', ' ', '1', ' ', 0
};

static void make_default_patch(uint8_t *dump)
{
    memcpy(dump, default_voice, VOICE_DUMP_SIZE);
}

typedef struct {
    int key;
    int semitone;
} key_note;

/* A tracker-style keyboard: the lower row starts at the base octave, the
 * upper row one octave above it. */
static const key_note key_map[] = {
    {'z', 0}, {'s', 1}, {'x', 2}, {'d', 3}, {'c', 4}, {'v', 5},
    {'g', 6}, {'b', 7}, {'h', 8}, {'n', 9}, {'j', 10}, {'m', 11},
    {',', 12},
    {'q', 12}, {'2', 13}, {'w', 14}, {'3', 15}, {'e', 16}, {'r', 17},
    {'5', 18}, {'t', 19}, {'6', 20}, {'y', 21}, {'7', 22}, {'u', 23},
    {'i', 24}
};

static int note_for_key(int key)
{
    size_t i;

    for (i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (key_map[i].key == key) {
            return key_map[i].semitone;
        }
    }
    return -1;
}

static int handle_key(slopsix_context ctx, int key, int *base_note,
    int held[128])
{
    int semitone;

    if (key == 'Q' || key == 27 /* Esc */ || key == 3 /* Ctrl-C */) {
        return 1;
    }
    if (key == '[') {
        if (*base_note > 12) {
            *base_note -= 12;
        }
        printf("octave: base note %d\n", *base_note);
        fflush(stdout);
        return 0;
    }
    if (key == ']') {
        if (*base_note < 96) {
            *base_note += 12;
        }
        printf("octave: base note %d\n", *base_note);
        fflush(stdout);
        return 0;
    }
    if (key == ' ') {
        slopsix_all_notes_off(ctx);
        memset(held, 0, 128u * sizeof(held[0]));
        return 0;
    }

    semitone = note_for_key(key);
    if (semitone < 0) {
        return 0;
    }

    {
        int note = *base_note + semitone;

        if (note < 0 || note > 127) {
            return 0;
        }
        if (held[note]) {
            slopsix_note_off(ctx, (uint8_t)note);
            held[note] = 0;
        } else {
            slopsix_note_on(ctx, (uint8_t)note, 100u);
            held[note] = 1;
        }
    }
    return 0;
}

static void fill_buffer(slopsix_context ctx, ALuint buffer, uint32_t sample_rate)
{
    int16_t pcm[BUFFER_FRAMES * 2u];

    slopsix_process(ctx, pcm, BUFFER_FRAMES);
    alBufferData(buffer, AL_FORMAT_STEREO16, pcm,
        (ALsizei)(BUFFER_FRAMES * 2u * sizeof(int16_t)),
        (ALsizei)sample_rate);
}

static void service_source(slopsix_context ctx, ALuint source,
    uint32_t sample_rate)
{
    ALint processed = 0;

    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint buffer = 0;

        alSourceUnqueueBuffers(source, 1, &buffer);
        if (buffer == 0) {
            break;
        }
        fill_buffer(ctx, buffer, sample_rate);
        alSourceQueueBuffers(source, 1, &buffer);
        processed--;
    }
}

static void start_stream(slopsix_context ctx, ALuint source,
    const ALuint *buffers, uint32_t sample_rate)
{
    unsigned int i;

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    for (i = 0; i < BUFFER_COUNT; i++) {
        fill_buffer(ctx, buffers[i], sample_rate);
        alSourceQueueBuffers(source, 1, &buffers[i]);
    }
    alSourcePlay(source);
}

int main(void)
{
    ALCdevice *device = 0;
    ALCcontext *al_context = 0;
    slopsix_context ctx = 0;
    ALuint source = 0;
    ALuint buffers[BUFFER_COUNT] = {0};
    uint32_t sample_rate = 49096u;
    uint8_t voice[VOICE_DUMP_SIZE];
    int held[128];
    int base_note = DEFAULT_BASE_NOTE;
    int quit = 0;
    int status = EXIT_FAILURE;

    printf("libslopsix terminal keyboard\n");
    printf("  Z S X D C V G B H N J M ,   lower octave\n");
    printf("  Q 2 W 3 E R 5 T 6 Y 7 U I   upper octave\n");
    printf("  [ ] octave   Space all notes off   Q or Esc quit\n\n");
    fflush(stdout);

    if (set_raw_mode() != 0) {
        return EXIT_FAILURE;
    }
    install_terminal_guards();

    memset(held, 0, sizeof(held));

    device = alcOpenDevice(0);
    if (device == 0) {
        fprintf(stderr, "Could not open an OpenAL device.\n");
        goto cleanup;
    }
    al_context = alcCreateContext(device, 0);
    if (al_context == 0) {
        fprintf(stderr, "Could not create an OpenAL context.\n");
        goto cleanup;
    }
    if (!alcMakeContextCurrent(al_context)) {
        fprintf(stderr, "Could not make the OpenAL context current.\n");
        goto cleanup;
    }

    {
        ALCint frequency = 0;

        alcGetIntegerv(device, ALC_FREQUENCY, 1, &frequency);
        if (frequency > 0) {
            sample_rate = (uint32_t)frequency;
        }
    }

    ctx = slopsix_create_context(sample_rate);
    if (ctx == 0) {
        fprintf(stderr, "Could not create the slopsix context.\n");
        goto cleanup;
    }

    make_default_patch(voice);
    if (!slopsix_set_voice(ctx, voice)) {
        fprintf(stderr, "Could not load the built-in voice.\n");
        goto cleanup;
    }

    alGenSources(1, &source);
    if (alGetError() != AL_NO_ERROR) {
        source = 0;
        fprintf(stderr, "Could not create an OpenAL source.\n");
        goto cleanup;
    }
    alGenBuffers(BUFFER_COUNT, buffers);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "Could not create OpenAL buffers.\n");
        goto cleanup;
    }

    printf("Sample rate: %u Hz (about %u ms of queued audio).\n\n",
        (unsigned)sample_rate,
        (unsigned)((BUFFER_FRAMES * BUFFER_COUNT * 1000u) / sample_rate));
    fflush(stdout);

    start_stream(ctx, source, buffers, sample_rate);

    while (!quit) {
        struct pollfd pfd;
        int ready;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char keys[64];
            ssize_t count = read(STDIN_FILENO, keys, sizeof(keys));
            ssize_t k;

            for (k = 0; k < count; k++) {
                if (handle_key(ctx, keys[k], &base_note, held)) {
                    quit = 1;
                    break;
                }
            }
            if ((pfd.revents & (POLLHUP | POLLERR)) && count <= 0) {
                quit = 1;
            }
        }

        {
            ALint state = 0;

            service_source(ctx, source, sample_rate);
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                alSourcePlay(source);
            }
        }
    }

    status = EXIT_SUCCESS;

cleanup:
    if (source != 0) {
        alSourceStop(source);
    }
    if (buffers[0] != 0) {
        alDeleteBuffers((ALsizei)BUFFER_COUNT, buffers);
    }
    if (source != 0) {
        alDeleteSources(1, &source);
    }
    if (ctx != 0) {
        slopsix_destroy_context(ctx);
    }
    if (al_context != 0) {
        alcMakeContextCurrent(0);
        alcDestroyContext(al_context);
    }
    if (device != 0) {
        alcCloseDevice(device);
    }
    if (terminal_is_raw) {
        restore_terminal();
        printf("\n");
    }
    return status;
}
