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
 * The composition is hypothesis-grade: the owner's manual gives the four
 * ranges and the two switches, but not their arithmetic, and the ROM
 * findings that would settle them (OQ-DX7S-04 and OQ-DX7S-06) are still
 * open.  The forms below are the simplest ones that fit the manual's
 * description, and they are written down as such.
 *
 * Unit domains:
 *
 *   - The envelope's level is the firmware's 16-bit word in the
 *     4096-per-octave domain, and the neutral value is 0x4000 (the
 *     shared envelope's own domain, plan decision D-12).  The range and
 *     the velocity switch act on the level's distance from that neutral
 *     value, so both work on the signed offset and not on the level.
 *   - The range's divisor is a power of two, so the division is a shift,
 *     and the engine floors it like a shift would (slopsix_floor_div)
 *     rather than truncating toward zero.
 *   - The velocity depth is a Q8 factor: 256 is the full depth and 0
 *     none, so the multiplication divides by 256, again a shift.
 *
 * This translation unit runs at control rate, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* The range's divisor, as the log2 of the factor the envelope's
 * excursion is divided by, indexed by the patch value (the public enum's
 * raw order, the DX7S data encoding: 0 = 8 octaves, 1 = 2 octaves, 2 =
 * 1 octave, 3 = 1/2 octave — the middle raw value's label is open
 * research, OQ-DX7S-03).  The divisors run against the Mk1's own
 * eight-octave span: raw 0 divides nothing, which makes the encoded
 * default the same value a zeroed envelope carries, and the half-octave
 * range at raw 3 divides by sixteen. */
static const uint8_t slopsix_dx7s_peg_range_shift[4] = {0u, 2u, 3u, 4u};

/*
 * The note-on composition.
 *
 * The velocity switch scales the envelope's depth with the key's
 * velocity, the range divides the envelope's excursion, and the rate
 * scaling speeds the stages up with the key.  The rate scaling is
 * applied to the stage increments the shared envelope loaded from the
 * ROM's rate table, so the stages keep their own rates and only move
 * faster.
 */
void slopsix_dx7s_pitch_eg_configure(const slopsix_patch_state *patch,
    slopsix_pitch_eg *eg, uint8_t note, uint8_t velocity)
{
    unsigned i;

    /* The velocity switch: with it on, the depth follows the key's
     * velocity linearly, from none at the softest key to the full depth
     * at the loudest.  With it off the depth is the envelope's own,
     * which is what the switch's off state means; the factor is then
     * not read at all. */
    if (patch->peg_velocity != 0u) {
        uint32_t value = (uint32_t)(velocity & 127u);

        eg->velocity_on = 1u;
        eg->velocity_scale_q8 = (uint16_t)((value * 256u + 63u) / 127u);
    } else {
        eg->velocity_on = 0u;
        eg->velocity_scale_q8 = 0u;
    }

    /* The range. */
    eg->range_shift = slopsix_dx7s_peg_range_shift[patch->peg_range & 3u];

    /* The rate scaling: the stages' increments grow with the key.  The
     * pitch envelope's own rate law is not in the manual, so the
     * operator envelope's rate-scaling shape is used as a provisional
     * one: the key's rate-scaling value (the operator law's, 0..31)
     * scales the increment, so a key at the top of the keyboard moves
     * the stage about twice as fast as the bottom one. */
    if (patch->peg_rate_scaling != 0u) {
        uint32_t rs = slopsix_rate_scaling(note, patch->peg_rate_scaling);

        for (i = 0; i < 4u; i++) {
            int32_t increment = ((int32_t)eg->increment[i] *
                (int32_t)(32u + rs) + 16) / 32;

            /* The increment is the ROM table's own 1..255 entry, so the
             * scaled value saturates at the table's top. */
            if (increment > 255) {
                increment = 255;
            }
            if (increment < 1) {
                increment = 1;
            }
            eg->increment[i] = (uint8_t)increment;
        }
    }
}

/*
 * The envelope's contribution to the voice's log-frequency word.
 *
 * The shared law (slopsix_pitch_eg_offset) is the level's distance from
 * the neutral value; the DX7S's law is that distance divided by the
 * range's divisor and then scaled by the velocity switch's depth factor.
 *
 * The range divides first, because it is a property of the envelope: the
 * panel's setting bounds the excursion the envelope can make.  The
 * velocity factor is at most one, so the key's depth can only narrow
 * that excursion further, never widen it.
 */
int32_t slopsix_dx7s_pitch_eg_offset(const slopsix_pitch_eg *eg)
{
    int32_t offset = eg->level - SLOPSIX_PITCH_EG_NEUTRAL_WORD;

    if (eg->range_shift != 0u) {
        offset = slopsix_floor_div(offset, eg->range_shift);
    }
    if (eg->velocity_on != 0u) {
        offset = slopsix_floor_div(
            offset * (int32_t)eg->velocity_scale_q8, 8);
    }

    return offset;
}
