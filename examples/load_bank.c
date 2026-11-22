/*
 * load_bank.c - select a voice from a DX7 SysEx bank and load it.
 *
 * A DX7 32-voice bank (.syx) is one SysEx message:
 *
 *   F0 43 00 09 20 00      header (6 bytes)
 *   32 x 128 bytes         packed voice data, program 0 first
 *   checksum               one byte
 *   F7                     end of SysEx
 *
 * Each packed voice is 17 bytes per operator (operator 6 first) followed
 * by 26 global bytes.  slopsix takes the unpacked 156-byte voice dump
 * that slopsix_set_voice() expects, so this example shows the whole
 * path: index the bank, unpack the packed voice, load it.
 *
 * The SysEx framing itself (checksum, F7) is a host concern and is only
 * checked at the bank-header level here.
 *
 * Usage:
 *   load_bank BANK.syx
 *       List the bank's 32 voices.
 *
 *   load_bank BANK.syx PROGRAM [OUT.wav]
 *       Load voice PROGRAM (0..31).  When OUT.wav is given, render a
 *       short C4 preview and write it as a stereo 16-bit WAV.
 *
 * Build: make example-bank
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slopsix.h"

#define BANK_SIZE 4104u
#define BANK_HEADER_SIZE 6u
#define PACKED_VOICE_SIZE 128u
#define PROGRAM_COUNT 32u
#define VOICE_DUMP_SIZE 156u
#define VOICE_NAME_OFFSET 118u
#define VOICE_NAME_LENGTH 10u

#define PREVIEW_RATE 49096u
#define PREVIEW_NOTE 60u
#define PREVIEW_VELOCITY 100u
#define PREVIEW_HOLD_FRAMES (PREVIEW_RATE * 3u / 4u)
#define PREVIEW_TAIL_FRAMES (PREVIEW_RATE * 3u / 4u)
#define PREVIEW_FRAMES (PREVIEW_HOLD_FRAMES + PREVIEW_TAIL_FRAMES)

static const unsigned char bank_header[BANK_HEADER_SIZE] = {
    0xF0, 0x43, 0x00, 0x09, 0x20, 0x00
};

static int read_bank(const char *path, unsigned char bank[BANK_SIZE])
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (f == NULL) {
        fprintf(stderr, "load_bank: cannot open %s\n", path);
        return 0;
    }
    got = fread(bank, 1, BANK_SIZE, f);
    fclose(f);
    if (got != BANK_SIZE) {
        fprintf(stderr, "load_bank: %s is not a %u-byte DX7 bank\n",
            path, (unsigned)BANK_SIZE);
        return 0;
    }
    if (memcmp(bank, bank_header, BANK_HEADER_SIZE) != 0) {
        fprintf(stderr, "load_bank: %s has no DX7 32-voice header\n",
            path);
        return 0;
    }
    return 1;
}

/* The program name sits at the end of the packed voice, padded with
 * spaces. */
static void voice_name(const unsigned char *bank, unsigned program,
    char name[VOICE_NAME_LENGTH + 1u])
{
    const unsigned char *packed = bank + BANK_HEADER_SIZE +
        program * PACKED_VOICE_SIZE;
    size_t i;

    for (i = 0; i < VOICE_NAME_LENGTH; i++) {
        unsigned char c = packed[VOICE_NAME_OFFSET + i] & 0x7Fu;

        name[i] = (c >= 32u && c < 127u) ? (char)c : ' ';
    }
    name[VOICE_NAME_LENGTH] = '\0';
    for (i = VOICE_NAME_LENGTH; i > 0 && name[i - 1u] == ' '; i--) {
        name[i - 1u] = '\0';
    }
}

/* Packed parameters are 7-bit.  Parameters whose own range is 0..99 are
 * stored directly; some editors export them scaled into 0..127, which is
 * normalised back here. */
static unsigned normparm(unsigned value, unsigned max)
{
    if (value <= max) {
        return value;
    }
    return value * max / 255u;
}

/* Unpack one 128-byte packed voice from the bank into the engine's
 * 156-byte dump.  The field order is the DX7 bulk-dump format: six
 * 17-byte operator blocks (operator 6 first), then the globals.  Keep
 * this conversion in step with the validation renderer's bank reader in
 * tools/validate_render.c; both encode the same packed format. */
static void unpack_voice(const unsigned char *bank, unsigned program,
    uint8_t out[VOICE_DUMP_SIZE])
{
    const unsigned char *bulk = bank + BANK_HEADER_SIZE +
        program * PACKED_VOICE_SIZE;
    unsigned op;
    unsigned i;

    for (op = 0; op < 6; op++) {
        for (i = 0; i < 11; i++) {
            out[op * 21 + i] = (uint8_t)normparm(
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
        out[126 + i] = (uint8_t)normparm(bulk[102 + i] & 0x7Fu, 99u);
    }
    out[134] = (uint8_t)normparm(bulk[110] & 0x1Fu, 31u);
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
    out[155] = 0x3Fu;
}

static void put32(FILE *f, unsigned long v)
{
    unsigned char b[4] = {
        (unsigned char)v, (unsigned char)(v >> 8),
        (unsigned char)(v >> 16), (unsigned char)(v >> 24)
    };

    fwrite(b, 1, sizeof(b), f);
}

static void put16(FILE *f, unsigned v)
{
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };

    fwrite(b, 1, sizeof(b), f);
}

static int write_wav(const char *path, const int16_t *stereo, size_t frames)
{
    FILE *f = fopen(path, "wb");
    unsigned long data_bytes = (unsigned long)frames * 4u;
    int ok = 1;
    size_t i;

    if (f == NULL) {
        fprintf(stderr, "load_bank: cannot write %s\n", path);
        return 0;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16u);
    put16(f, 1u);                  /* PCM */
    put16(f, 2u);                  /* stereo */
    put32(f, PREVIEW_RATE);
    put32(f, PREVIEW_RATE * 4u);   /* byte rate */
    put16(f, 4u);                  /* block align */
    put16(f, 16u);                 /* bits per sample */
    fwrite("data", 1, 4, f);
    put32(f, data_bytes);
    for (i = 0; i < frames * 2u; i++) {
        put16(f, (unsigned)(uint16_t)stereo[i]);
    }
    if (ferror(f)) {
        ok = 0;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "load_bank: error writing %s\n", path);
    }
    return ok;
}

static int parse_program(const char *text, unsigned *program)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < 0 ||
        value >= (long)PROGRAM_COUNT) {
        fprintf(stderr, "load_bank: program must be 0..%u\n",
            (unsigned)(PROGRAM_COUNT - 1u));
        return 0;
    }
    *program = (unsigned)value;
    return 1;
}

static void list_voices(const unsigned char *bank)
{
    unsigned program;

    for (program = 0; program < PROGRAM_COUNT; program++) {
        char name[VOICE_NAME_LENGTH + 1u];

        voice_name(bank, program, name);
        printf("%2u  %s\n", program, name);
    }
}

static int preview(slopsix_context ctx, const char *path,
    const uint8_t voice[VOICE_DUMP_SIZE])
{
    int16_t *pcm = malloc(PREVIEW_FRAMES * 2u * sizeof(*pcm));
    slopsix_error err;

    if (pcm == NULL) {
        fprintf(stderr, "load_bank: out of memory\n");
        return 0;
    }

    err = slopsix_set_voice(ctx, voice, VOICE_DUMP_SIZE);
    if (err != SLOPSIX_OK) {
        fprintf(stderr, "load_bank: slopsix_set_voice failed (%d)\n", err);
        free(pcm);
        return 0;
    }

    err = slopsix_note_on(ctx, PREVIEW_NOTE, PREVIEW_VELOCITY);
    if (err != SLOPSIX_OK) {
        fprintf(stderr, "load_bank: slopsix_note_on failed (%d)\n", err);
        free(pcm);
        return 0;
    }
    slopsix_process(ctx, pcm, PREVIEW_HOLD_FRAMES);
    err = slopsix_note_off(ctx, PREVIEW_NOTE);
    if (err != SLOPSIX_OK) {
        fprintf(stderr, "load_bank: slopsix_note_off failed (%d)\n", err);
        free(pcm);
        return 0;
    }
    slopsix_process(ctx, pcm + PREVIEW_HOLD_FRAMES * 2u, PREVIEW_TAIL_FRAMES);

    if (!write_wav(path, pcm, PREVIEW_FRAMES)) {
        free(pcm);
        return 0;
    }
    free(pcm);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char bank[BANK_SIZE];
    uint8_t voice[VOICE_DUMP_SIZE];
    char name[VOICE_NAME_LENGTH + 1u];
    slopsix_context ctx;
    unsigned program;
    int status = EXIT_FAILURE;

    if (argc < 2 || argc > 4) {
        fprintf(stderr,
            "usage: load_bank BANK.syx [PROGRAM [OUT.wav]]\n");
        return EXIT_FAILURE;
    }
    if (!read_bank(argv[1], bank)) {
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        list_voices(bank);
        return EXIT_SUCCESS;
    }
    if (!parse_program(argv[2], &program)) {
        return EXIT_FAILURE;
    }

    voice_name(bank, program, name);
    unpack_voice(bank, program, voice);

    ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, PREVIEW_RATE);
    if (ctx == 0) {
        fprintf(stderr, "load_bank: could not create a slopsix context\n");
        return EXIT_FAILURE;
    }
    if (slopsix_voice_dump_size(ctx) != VOICE_DUMP_SIZE) {
        fprintf(stderr, "load_bank: unexpected DX7 dump size\n");
        goto cleanup;
    }

    if (argc == 4) {
        if (!preview(ctx, argv[3], voice)) {
            goto cleanup;
        }
    } else {
        slopsix_error err = slopsix_set_voice(ctx, voice, VOICE_DUMP_SIZE);

        if (err != SLOPSIX_OK) {
            fprintf(stderr, "load_bank: slopsix_set_voice failed (%d)\n",
                err);
            goto cleanup;
        }
    }

    printf("loaded program %u \"%s\"\n", program, name);
    if (argc == 4) {
        printf("wrote %s\n", argv[3]);
    }
    status = EXIT_SUCCESS;

cleanup:
    slopsix_destroy_context(ctx);
    return status;
}
