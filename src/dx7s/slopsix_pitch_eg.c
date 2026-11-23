/*
 * src/dx7s/slopsix_pitch_eg.c - the DX7S's pitch-envelope law.
 *
 * The DX7S composes three parameters into its pitch envelope that the
 * Mk1's firmware does not have (plan D7S-4): the range, the velocity
 * switch and the rate scaling.  This file holds that composition; the
 * shared envelope (src/slopsix_pitch_eg.c) holds the Mk1's own law and
 * knows nothing of it.
 *
 * The personality's law is reached through the context's two hooks
 * (state->pitch_eg_configure and state->pitch_eg_offset), which the
 * personality init resolves, exactly as it resolves the output stage and
 * the voice bus.  A DX7 context in a DX7S build therefore plays the
 * firmware's envelope whatever the shared patch state holds, and a DX7S
 * context with the neutral parameters plays the firmware's envelope too.
 *
 * The DX7S pitch arithmetic is as follows:
 *
 *   - the range divides the signed level byte's distance from 0x80 by a
 *     power of four (shifts 7/5/3/1 over raw 0..3, spans 8/2/0.5/0.125
 *     octaves), neutral staying at 0x4000;
 *   - the velocity switch scales both the level depth and the stage
 *     rate by (velocity + 1) / 128, with the level's own +1 bias; the
 *     0xFF rate sentinel is left untouched;
 *   - the rate scaling is additive on the composed rate byte:
 *     `(max(note - 26, 0) >> 2) * depth`, clamped to 0xFE, and it leaves
 *     the 0xFF sentinel untouched;
 *   - the note-on process moves the level by twice the composed rate
 *     byte, and starts at level 1 when the first composed rate is 0xFF.
 *
 * Unit domains:
 *
 *   - The envelope's level is the firmware's 16-bit word in the
 *     4096-per-octave domain, and the neutral value is 0x4000 (the
 *     shared envelope's own domain, plan decision D-12).
 *   - The firmware's rate/level tables yield bytes; this law composes
 *     the level byte and writes the resulting word into the shared
 *     target, and writes twice the composed rate byte into the shared
 *     increment, so the shared stage walk itself runs in the hardware's
 *     composed domain and its offset is the plain distance from neutral.
 *
 * This translation unit runs at control rate, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* The ROM rate table's top byte is an instant sentinel. */
#define SLOPSIX_DX7S_PEG_RATE_SENTINEL 0xFFu
#define SLOPSIX_DX7S_PEG_RATE_TOP      0xFEu
#define SLOPSIX_DX7S_PEG_LEVEL_NEUTRAL 0x80

/*
 * The velocity path of the level-byte scaling. It is deliberately
 * written in the 8-bit register steps: the byte is converted to its
 * signed offset, made positive, biased by one, multiplied by velocity+1,
 * shifted left once and its high byte taken, sign-restored and converted
 * back. The conversion wraps in 8 bits at the extreme byte values. Level
 * 0x00 at the top velocity becomes 0xFF, level 0xFF becomes 0x00.
 */
static uint8_t slopsix_dx7s_peg_level_velocity(uint8_t level_byte,
    uint8_t velocity)
{
    uint8_t a = (uint8_t)(level_byte - SLOPSIX_DX7S_PEG_LEVEL_NEUTRAL);
    uint8_t b = (uint8_t)(velocity + 1u);
    uint16_t product;

    if ((a & 0x80u) != 0u) {
        a = (uint8_t)(0u - a);      /* NEGA */
        a = (uint8_t)(a + 1u);      /* INCA */
        product = (uint16_t)a * b;  /* MUL */
        product = (uint16_t)(product << 1u); /* ASLD */
        a = (uint8_t)(product >> 8u);
        a = (uint8_t)(0u - a);      /* NEGA */
    } else {
        a = (uint8_t)(a + 1u);      /* INCA */
        product = (uint16_t)a * b;  /* MUL */
        product = (uint16_t)(product << 1u);
        a = (uint8_t)(product >> 8u);
    }

    return (uint8_t)(a + SLOPSIX_DX7S_PEG_LEVEL_NEUTRAL); /* ADDA #0x80 */
}

/*
 * Compose one level byte into the shared envelope's word target.
 *
 * The velocity switch first scales the byte (above); the range then
 * applies the signed shift, which divides the word's excursion by four
 * per raw step (the scale factors `2^(7 - 2*range)`).
 */
static int32_t slopsix_dx7s_peg_compose_level(uint8_t level_byte,
    uint8_t range, uint8_t velocity, int velocity_on)
{
    int32_t offset;
    int32_t magnitude;
    unsigned shift;

    if (velocity_on != 0) {
        level_byte = slopsix_dx7s_peg_level_velocity(level_byte, velocity);
    }

    offset = (int32_t)level_byte - SLOPSIX_DX7S_PEG_LEVEL_NEUTRAL;
    shift = 7u - 2u * (unsigned)(range & 3u);
    magnitude = offset < 0 ? -offset : offset;
    magnitude *= (int32_t)(1u << shift);

    return SLOPSIX_PITCH_EG_NEUTRAL_WORD +
        (offset < 0 ? -magnitude : magnitude);
}

/*
 * Compose one rate byte: the velocity switch's speed scaling first, then
 * the additive rate scaling.  Both leave the 0xFF sentinel untouched.
 */
static uint8_t slopsix_dx7s_peg_compose_rate(uint8_t rate, uint8_t velocity,
    int velocity_on, uint8_t note, uint8_t depth)
{
    uint32_t value = rate;

    if (velocity_on != 0 && value != SLOPSIX_DX7S_PEG_RATE_SENTINEL) {
        value = (value * ((uint32_t)velocity + 1u) * 2u) >> 8;
    }

    if (depth != 0u && value != SLOPSIX_DX7S_PEG_RATE_SENTINEL) {
        uint32_t group = note > 26u ? ((uint32_t)note - 26u) >> 2 : 0u;
        uint32_t delta = group * (uint32_t)depth;

        if (delta > 0xFFu) {
            delta = 0xFFu;
        }
        value += delta;
        if (value >= SLOPSIX_DX7S_PEG_RATE_SENTINEL) {
            value = SLOPSIX_DX7S_PEG_RATE_TOP;
        }
    }

    return (uint8_t)value;
}

/*
 * The note-on composition.
 *
 * The shared init has already placed the level and rate bytes in the
 * targets and increments. This hook composes them in place, so the
 * shared stage walk runs in the hardware's composed domain and the
 * offset hook stays the plain distance from neutral.
 */
void slopsix_dx7s_pitch_eg_configure(const slopsix_patch_state *patch,
    slopsix_pitch_eg *eg, uint8_t note, uint8_t velocity)
{
    uint8_t range = patch->peg_range & 3u;
    uint8_t depth = patch->peg_rate_scaling & 7u;
    uint8_t key_velocity = velocity & 127u;
    int velocity_on = patch->peg_velocity != 0u;
    uint8_t first_rate = 0u;
    unsigned i;

    for (i = 0; i < 4u; i++) {
        uint8_t level_byte = (uint8_t)(eg->target[i] >> 7);
        uint8_t rate = slopsix_dx7s_peg_compose_rate(
            (uint8_t)eg->increment[i], key_velocity, velocity_on, note,
            depth);

        if (i == 0u) {
            first_rate = rate;
        }

        eg->target[i] = slopsix_dx7s_peg_compose_level(level_byte, range,
            key_velocity, velocity_on);
        /* The DX7S process moves by twice the composed rate byte. */
        eg->increment[i] = (uint16_t)rate * 2u;
    }

    eg->level = eg->target[3];
    eg->start_at_first =
        (first_rate == SLOPSIX_DX7S_PEG_RATE_SENTINEL) ? 1u : 0u;
}

/*
 * The envelope's contribution to the voice's log-frequency word. Both
 * the range and the velocity switch are already baked into the level and
 * the targets, so the contribution is simply the level's distance from
 * the neutral word.
 */
int32_t slopsix_dx7s_pitch_eg_offset(const slopsix_pitch_eg *eg)
{
    return eg->level - SLOPSIX_PITCH_EG_NEUTRAL_WORD;
}
