/*
 * slopsix_bend.c - The pitch bend: the firmware's curve, its quantisation
 * and the pitch-modification word the firmware computes from them.
 *
 * The bend is a global, static offset: the firmware computes it once per
 * tick and pushes it to the EGS's pitch-modification register, which the
 * chip adds to the voice's frequency (research P-05, OQ-03).
 *
 * The routine below is the firmware's own bend parse, ported instruction
 * for instruction, so that the hardware's quirks survive:
 *
 *   - the input is reduced to the hardware's seven bits (the bend's bits
 *     13..7), so the bend is a staircase of 128 steps;
 *   - the curve table compresses the middle of the travel and holds three
 *     input steps at the neutral value: the centre dead zone;
 *   - two of the curve's values are never produced by any input;
 *   - the down side is one step short of the up side (the |s|-1
 *     compression) and reaches one step further;
 *   - the range-12 case has its own path, in which the top sample is
 *     special;
 *   - a non-zero step ignores the range and quantises the bend into the
 *     step table's staircase.
 *
 * Unit domains (plan decision D-12):
 *
 *   - The word this file produces is the EGS's pitch-modification word:
 *     16 bits in units of 1/16384 of an octave, that is, two fractional
 *     bits below the EGS's 1024-per-octave frequency datum.
 *   - The engine's own log-frequency word is the firmware's
 *     4096-per-octave word, so the pitch mod enters it shifted right by
 *     two.  Both quantities are the same pitch, in different chips'
 *     domains; the conversion is a shift, and the two units are named at
 *     the point where they meet.
 *
 * This translation unit runs at control rate (the bend is recomputed when
 * the bend, the range or the step changes), so it is compiled size-first.
 */
#include "slopsix_internal.h"

/* The bend curve: the firmware's 128-entry input curve, indexed by the
 * reduced (seven-bit) bend.  The curve is the identity over most of its
 * travel, compresses the middle (indices 41..63 map to 42..64 and
 * 65..87 map to 64..86) and holds the neutral value 64 for the three
 * input steps 63, 64 and 65 - the centre dead zone.  The values 41 and
 * 87 are never produced by any input. */
static const uint8_t slopsix_bend_curve[128] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    64, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
    79, 80, 81, 82, 83, 84, 85, 86, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
    123, 124, 125, 126, 127
};

/* The cents table: the continuous mode's per-range scale, thirteen
 * entries, indexed by the bend range (0..12).  Entry r is about
 * r * 255/12. */
static const uint8_t slopsix_bend_cents[13] = {
    0, 22, 43, 65, 86, 107, 129, 151, 172, 194, 215, 236, 255
};

/* The step table: sixteen groups of three bytes, indexed by the step
 * parameter (0..15) times three.  Each group is a count multiplier and a
 * 16-bit step word. */
static const uint8_t slopsix_bend_step[48] = {
    0x00, 0x00, 0x00, 0x19, 0x05, 0x55, 0x0D, 0x0A, 0xAA, 0x09, 0x10, 0x00,
    0x07, 0x15, 0x55, 0x05, 0x1A, 0xAA, 0x05, 0x20, 0x00, 0x03, 0x25, 0x55,
    0x03, 0x2A, 0xAA, 0x03, 0x30, 0x00, 0x03, 0x35, 0x55, 0x03, 0x3A, 0xAA,
    0x03, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* The number of the bend's reduced steps: the firmware's input is the
 * bend's most significant byte shifted left by one, and the parse shifts
 * it right again, so the curve's index is the bend's bits 13..7. */
#define SLOPSIX_BEND_INDEX_SHIFT 7

/* The range at which the firmware takes its own path. */
#define SLOPSIX_BEND_RANGE_SPECIAL 12u

/*
 * The pitch-modification word.
 *
 * The firmware's bend parse, in the EGS's pitch-modification units
 * (1/16384 of an octave).  The port keeps the firmware's register
 * arithmetic - the eight-bit accumulator, the sixteen-bit complement and
 * the logical shifts - because the quirks live in them.
 */
int32_t slopsix_bend_pitch_mod(uint16_t bend14, uint8_t range, uint8_t step)
{
    uint32_t index;
    uint32_t curve;
    uint32_t a;
    uint32_t b;
    uint32_t d;
    uint32_t product;
    uint32_t count;
    uint32_t word;
    uint32_t i;
    int32_t x;

    /* The reduced bend: the firmware's seven-bit curve index. */
    index = ((uint32_t)bend14 >> SLOPSIX_BEND_INDEX_SHIFT) & 0x7Fu;

    /* The hardware validates the range to 0..12 and clears a larger one,
     * which is the same as no bend at all. */
    if (range > 12u) {
        range = 0u;
    }

    curve = slopsix_bend_curve[index];

    /* The signed bend value: the curve's value shifted left by one, with
     * the sign bit inverted, so the byte reads as 2 * (curve - 64). */
    a = ((curve << 1) & 0xFFu) ^ 0x80u;
    x = (a & 0x80u) ? ((int32_t)a - 256) : (int32_t)a;

    if (step != 0u) {
        /* The step path: the range is ignored and the bend is quantised
         * into the step table's staircase. */
        b = (uint32_t)step & 0x0Fu;

        /* The negative branch complements the bend value, which is the
         * |s|-1 step: the down side is one step short of the up side. */
        if (x < 0) {
            a = (~(uint32_t)x) & 0xFFu;
        } else {
            a = (uint32_t)x & 0xFFu;
        }

        /* The multiplier and the step word of the group. */
        product = a * (uint32_t)slopsix_bend_step[b * 3u];
        count = product >> 8;

        if (count == 0u) {
            /* The product's high byte is zero: the bend contributes
             * nothing at this deflection. */
            return 0;
        }

        /* The step word is big-endian, as the processor's words are. */
        word = ((uint32_t)slopsix_bend_step[b * 3u + 1u] << 8) |
               (uint32_t)slopsix_bend_step[b * 3u + 2u];

        d = word;

        /* The firmware adds the word once per step, count times. */
        for (i = 1u; i < count; i++) {
            d += word;
        }

        if (x < 0) {
            /* The negative branch one's-complements the result. */
            d = (~d) & 0xFFFFu;
        }

        return (int32_t)(int16_t)(uint16_t)d;
    }

    if (range == SLOPSIX_BEND_RANGE_SPECIAL) {
        /* The range-12 path: the bend's own scale, with the top sample
         * special. */
        if (x >= 0) {
            d = (x == 126) ? 0x7FFFu : ((uint32_t)x << 8);
            /* The shift left and the two shifts right halve the top
             * sample's 0x7FFF into 0x3FFF. */
            d = (d << 1) & 0xFFFFu;
            d >>= 2;
            return (int32_t)(int16_t)(uint16_t)d;
        }

        /* The negative branch: complement, rotate, complement again. */
        a = (~(uint32_t)x) & 0xFFu;
        a = ((a << 1) | 1u) & 0xFFu;
        a = (~a) & 0xFFu;
        d = (a << 8) & 0xFFFFu;
        d >>= 2;
        /* The sign extension: the magnitude sits in the low bits and the
         * high byte carries the sign. */
        d |= 0xC000u;
        return (int32_t)(int16_t)(uint16_t)d;
    }

    /* The continuous path: the range's cents value scales the bend. */
    b = slopsix_bend_cents[range];

    if (x >= 0) {
        a = ((uint32_t)x << 1) & 0xFFu;
        product = a * b;
        d = (product >> 2) & 0xFFFFu;
        return (int32_t)(int16_t)(uint16_t)d;
    }

    /* The negative branch: the complement of the bend value (the |s|-1
     * step), the product, then the sixteen-bit one's complement of the
     * product and the two shifts right. */
    a = (~(uint32_t)x) & 0xFFu;
    a = (a << 1) & 0xFFu;
    product = a * b;
    d = (~product) & 0xFFFFu;
    d >>= 2;
    d |= 0xC000u;
    return (int32_t)(int16_t)(uint16_t)d;
}

/*
 * The bend's contribution to the voice's log-frequency word.
 *
 * The pitch-modification word is in the EGS's 1/16384-octave units and
 * the engine's word is the firmware's 4096-per-octave word, so the pitch
 * mod is shifted right by two on its way in (plan decision D-12).  The
 * shift is a floor division, so a bend downwards keeps its sign.
 */
int32_t slopsix_bend_log_offset(uint16_t bend14, uint8_t range, uint8_t step)
{
    return slopsix_floor_div(slopsix_bend_pitch_mod(bend14, range, step),
                             SLOPSIX_PITCH_MOD_TO_LOG_SHIFT);
}

/*
 * The bend's contribution for a context.
 *
 * The bend, its range and its step are global, as on the hardware: every
 * voice reads the same value.  The voice path calls this when it composes
 * a voice's frequency.
 */
int32_t slopsix_bend_context_offset(const slopsix_context_state *state)
{
    return slopsix_bend_log_offset(state->pitch_bend, state->bend_range,
                                    state->bend_step);
}
