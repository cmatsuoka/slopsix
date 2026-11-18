/*
 * validate_render.c - render one DX7 bank voice with libslopsix.
 *
 * This is the slopsix side of the validation loop: it takes the same
 * command line as the external reference renderers and writes a mono WAV
 * at the requested sample rate.  The voice is unpacked from the bank's
 * packed 128-byte format into the engine's 156-byte dump.
 *
 * Build: make validate
 * Usage: validate_render --bank BANK.syx --program N --note N
 *                        --velocity N --hold SECONDS --tail SECONDS
 *                        --rate HZ --out FILE.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slopsix.h"

struct args {
    const char *bank;
    const char *out;
    int program;
    int note;
    int velocity;
    int rate;
    double hold;
    double tail;
};

static int parse_args(int argc, char **argv, struct args *a)
{
    int i;

    a->bank = NULL;
    a->out = NULL;
    a->program = 0;
    a->note = 60;
    a->velocity = 100;
    a->rate = 49096;
    a->hold = 1.0;
    a->tail = 1.0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bank") == 0 && i + 1 < argc) {
            a->bank = argv[++i];
        } else if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            a->program = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) {
            a->note = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--velocity") == 0 && i + 1 < argc) {
            a->velocity = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            a->rate = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            a->hold = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            a->tail = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            a->out = argv[++i];
        } else {
            fprintf(stderr, "validate_render: unknown argument %s\n",
                argv[i]);
            return 0;
        }
    }
    if (a->bank == NULL || a->out == NULL) {
        fprintf(stderr,
            "validate_render: --bank and --out are required\n");
        return 0;
    }
    return 1;
}

static unsigned normparm(unsigned value, unsigned max)
{
    if (value <= max) {
        return value;
    }
    return value * max / 255u;
}

static int unpack_voice(const unsigned char *bank, int program,
    unsigned char out[156])
{
    const unsigned char *bulk = bank + 6 + program * 128;
    int op;
    int i;

    for (op = 0; op < 6; op++) {
        for (i = 0; i < 11; i++) {
            out[op * 21 + i] = (unsigned char)normparm(
                bulk[op * 17 + i] & 0x7Fu, 99u);
        }
        {
            unsigned char curves = bulk[op * 17 + 11] & 0x0Fu;
            unsigned char det_rs = bulk[op * 17 + 12] & 0x7Fu;
            unsigned char kvs_ams = bulk[op * 17 + 13] & 0x1Fu;
            unsigned char coarse_mode = bulk[op * 17 + 15] & 0x3Fu;

            out[op * 21 + 11] = curves & 3u;
            out[op * 21 + 12] = (curves >> 2) & 3u;
            out[op * 21 + 13] = det_rs & 7u;
            out[op * 21 + 14] = kvs_ams & 3u;
            out[op * 21 + 15] = (kvs_ams >> 2) & 7u;
            out[op * 21 + 16] = bulk[op * 17 + 14] & 0x7Fu;
            out[op * 21 + 17] = coarse_mode & 1u;
            out[op * 21 + 18] = (coarse_mode >> 1) & 0x1Fu;
            out[op * 21 + 19] = bulk[op * 17 + 16] & 0x7Fu;
            out[op * 21 + 20] = (det_rs >> 3) & 0x7Fu;
        }
    }
    for (i = 0; i < 8; i++) {
        out[126 + i] = (unsigned char)normparm(bulk[102 + i] & 0x7Fu, 99u);
    }
    out[134] = (unsigned char)normparm(bulk[110] & 0x1Fu, 31u);
    {
        unsigned char oks_fb = bulk[111] & 0x0Fu;
        unsigned char lpms_lfw_lks = bulk[116] & 0x7Fu;

        out[135] = oks_fb & 7u;
        out[136] = oks_fb >> 3;
        out[137] = bulk[112] & 0x7Fu;
        out[138] = bulk[113] & 0x7Fu;
        out[139] = bulk[114] & 0x7Fu;
        out[140] = bulk[115] & 0x7Fu;
        out[141] = lpms_lfw_lks & 1u;
        out[142] = (lpms_lfw_lks >> 1) & 7u;
        out[143] = lpms_lfw_lks >> 4;
        out[144] = bulk[117] & 0x7Fu;
    }
    for (i = 0; i < 10; i++) {
        out[145 + i] = bulk[118 + i] & 0x7Fu;
    }
    out[155] = 0x3F;
    return 1;
}

static void put32(FILE *f, unsigned long v)
{
    unsigned char b[4] = {
        (unsigned char)v, (unsigned char)(v >> 8),
        (unsigned char)(v >> 16), (unsigned char)(v >> 24)
    };
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, unsigned v)
{
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static int write_wav(const char *path, const short *samples, size_t frames,
    unsigned rate)
{
    FILE *f = fopen(path, "wb");
    unsigned long data_bytes = (unsigned long)frames * 2u;
    size_t i;

    if (f == NULL) {
        return 0;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16u);
    put16(f, 1u);
    put16(f, 1u);
    put32(f, rate);
    put32(f, rate * 2u);
    put16(f, 2u);
    put16(f, 16u);
    fwrite("data", 1, 4, f);
    put32(f, data_bytes);
    for (i = 0; i < frames; i++) {
        put16(f, (unsigned short)samples[i]);
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    struct args a;
    unsigned char bank[4104];
    unsigned char voice[156];
    int16_t stereo[2];
    short *out;
    slopsix_context ctx;
    FILE *f;
    size_t frames;
    size_t hold_frames;
    size_t i;

    if (!parse_args(argc, argv, &a)) {
        return 2;
    }
    if (a.program < 0 || a.program >= 32) {
        fprintf(stderr, "validate_render: program out of range\n");
        return 2;
    }
    f = fopen(a.bank, "rb");
    if (f == NULL) {
        fprintf(stderr, "validate_render: cannot open %s\n", a.bank);
        return 2;
    }
    if (fread(bank, 1, sizeof(bank), f) != sizeof(bank)) {
        fprintf(stderr, "validate_render: %s is not a 4104-byte bank\n",
            a.bank);
        fclose(f);
        return 2;
    }
    fclose(f);
    if (memcmp(bank, "\xf0\x43\x00\x09\x20\x00", 6) != 0) {
        fprintf(stderr, "validate_render: %s has no DX7 bank header\n",
            a.bank);
        return 2;
    }

    unpack_voice(bank, a.program, voice);

    frames = (size_t)((a.hold + a.tail) * (double)a.rate + 0.5);
    hold_frames = (size_t)(a.hold * (double)a.rate + 0.5);
    out = (short *)calloc(frames, sizeof(*out));
    if (out == NULL) {
        return 3;
    }

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, (uint32_t)a.rate);
    if (ctx == NULL) {
        free(out);
        return 3;
    }
    slopsix_set_voice(ctx, voice, sizeof(voice));
    slopsix_note_on(ctx, (uint8_t)a.note, (uint8_t)a.velocity);

    for (i = 0; i < frames; i++) {
        if (i == hold_frames) {
            slopsix_note_off(ctx, (uint8_t)a.note);
        }
        slopsix_process(ctx, stereo, 1);
        out[i] = (short)stereo[0];
    }

    slopsix_destroy_context(ctx);

    if (!write_wav(a.out, out, frames, (unsigned)a.rate)) {
        free(out);
        fprintf(stderr, "validate_render: cannot write %s\n", a.out);
        return 4;
    }
    free(out);
    return 0;
}
