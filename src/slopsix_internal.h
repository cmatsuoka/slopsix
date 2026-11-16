/*
 * slopsix_internal.h - internal structures shared by the library sources.
 *
 * Nothing here is public; the only public header is include/slopsix.h.
 */
#ifndef SLOPSIX_INTERNAL_H
#define SLOPSIX_INTERNAL_H

#include "slopsix.h"

/* --------------------------------------------------------------- constants */

/* Voices and operators. */
#define SLOPSIX_VOICE_COUNT 16
#define SLOPSIX_OP_COUNT    6

/* The engine's anchor rate; the tables and rate constants derive from it
 * at context creation (research F-04, algorithm page section 1). */
#define SLOPSIX_DEFAULT_SAMPLE_RATE 49096u

/* The DX7 patch dump size, in bytes. */
#define SLOPSIX_VOICE_DUMP_SIZE 156

/* Log-frequency word: the firmware's own 16-bit, 4096-units-per-octave
 * word.  The EGS's frequency field is the same quantity at 1024 units per
 * octave (word >> 2), so the firmware's constants appear unscaled here:
 * the frequency bias is 0x1BA8 and the master tune is `value << 1`.
 * See the library page section 10 and plan decision D-12. */
#define SLOPSIX_LOG_UNITS_PER_OCTAVE 4096

/* The EGS's 14-bit frequency field, 1024 units per octave. */
#define SLOPSIX_EGS_UNITS_PER_OCTAVE 1024

/* The hardware's frequency bias, subtracted from the log-frequency word
 * before the master tune is added (firmware section 3). */
#define SLOPSIX_FREQ_BIAS 0x1BA8

/* One unit of the Q16 public scales is 65536. */
#define SLOPSIX_Q16_ONE 65536

/* ----------------------------------------------------------- voice state */

typedef enum {
    SLOPSIX_VOICE_FREE = 0,
    SLOPSIX_VOICE_ON,
    SLOPSIX_VOICE_RELEASED
} slopsix_voice_state;

typedef struct {
    slopsix_voice_state state;
    bool held;          /* key still down; false while pedal-sustained */
    uint8_t note;       /* 0..127 */
    uint8_t velocity;   /* 0..127 */
} slopsix_voice;

/* --------------------------------------------------------- context state */

typedef struct {
    uint32_t sample_rate;

    /* Pitch. */
    uint16_t pitch_bend;        /* 14-bit, 0x2000 centred */
    uint8_t bend_range;         /* semitones, 0..12 */
    uint8_t bend_step;          /* 0..15 */
    int32_t master_tune_q16;    /* cents, 0 == shipped tuning */

    /* Mix. */
    int32_t pan_q16;            /* 32768 == centre */
    unsigned master_volume;     /* 0..7 */

    /* Voice management. */
    bool sustain;
    bool mono;

    /* Controllers. */
    uint8_t ctrl_value[SLOPSIX_CTRL_SRC_COUNT];  /* 0..127 */
    uint8_t ctrl_range[SLOPSIX_CTRL_SRC_COUNT];  /* 0..99 */
    unsigned ctrl_assign[SLOPSIX_CTRL_SRC_COUNT];

    /* Voices and the live patch block. */
    slopsix_voice voices[SLOPSIX_VOICE_COUNT];
    uint8_t voice_dump[SLOPSIX_VOICE_DUMP_SIZE];
} slopsix_context_state;

/* The context handle is the context state; the public API uses void *. */
#define SLOPSIX_CONTEXT(ctx) ((slopsix_context_state *)(ctx))

#endif /* SLOPSIX_INTERNAL_H */
