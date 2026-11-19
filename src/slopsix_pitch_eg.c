/*
 * slopsix_pitch_eg.c - The pitch envelope: the firmware's two quantization
 * tables, its stage walk and the level's contribution to the voice's
 * log-frequency word.
 *
 * The pitch EG is a CPU-side, per-voice control-rate process (research
 * E-04, plan decision D-1): the firmware runs PITCH_EG_PROCESS every
 * second OCF tick, i.e. at about 187.6 Hz, and folds the current level
 * into the voice's frequency word.  The engine keeps that staircase; it
 * does not interpolate the level per sample.
 *
 * The update's cadence is not in this file: the firmware's toggle is
 * global, so the gate lives with the context (src/slopsix_internal.h)
 * and this file only performs the per-voice update.  The envelope
 * therefore cannot be stepped twice for one tick.
 *
 * Unit domains (plan decision D-12):
 *
 *   - The firmware's pitch-EG level is a 16-bit word in the same
 *     4096-per-octave domain as the note word.  The patch's 0..99 level
 *     parameter is first quantized through TABLE_PITCH_EG_LEVEL (0..255),
 *     then stored as `value << 7`; the neutral value is 0x80 << 7 =
 *     0x4000, so the level's contribution is its distance from 0x4000.
 *     The engine stores the same 16-bit level and returns
 *     `level - 0x4000`, a signed offset in the log-frequency word.
 *   - The firmware's increment is the TABLE_PITCH_EG_RATE entry itself
 *     (1..255), added once per pitch-EG update.  The table is in the
 *     level's 16-bit units, so no conversion is needed between the two.
 *
 * The level table is the firmware's, verbatim; subtracting 0x80 gives the
 * familiar signed -128..+127 table, but the engine keeps the unsigned
 * firmware domain so that the stage comparisons below can reproduce the
 * firmware's high-byte snapping exactly.
 *
 * This translation unit runs at control rate, so it is compiled
 * size-first.
 */
#include <string.h>

#include "slopsix_internal.h"

/* The firmware's TABLE_PITCH_EG_RATE: the 100-entry quantization of the
 * patch's rate parameters to the increments the pitch EG applies.  The
 * table is nonlinear; its first entry is 1 and its last is 255. */
static const uint8_t slopsix_pitch_eg_rate[100] = {
    0x01, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
    0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x0A, 0x0A, 0x0B, 0x0B,
    0x0C, 0x0C, 0x0D, 0x0D, 0x0E, 0x0E, 0x0F, 0x10, 0x10, 0x11,
    0x12, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
    0x1B, 0x1C, 0x1E, 0x1F, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27,
    0x29, 0x2A, 0x2C, 0x2E, 0x2F, 0x31, 0x33, 0x35, 0x36, 0x38,
    0x3A, 0x3C, 0x3E, 0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C,
    0x4F, 0x52, 0x55, 0x58, 0x5B, 0x5E, 0x62, 0x66, 0x6A, 0x6E,
    0x73, 0x78, 0x7D, 0x82, 0x87, 0x8D, 0x93, 0x99, 0x9F, 0xA5,
    0xAB, 0xB2, 0xB9, 0xC1, 0xCA, 0xD3, 0xE8, 0xF3, 0xFE, 0xFF
};

/* The firmware's TABLE_PITCH_EG_LEVEL: the 100-entry quantization of the
 * patch's level parameters to 0..255.  Entry 50 is 0x80, the neutral
 * level; the top of the table is nonlinear. */
static const uint8_t slopsix_pitch_eg_level[100] = {
    0x00, 0x0C, 0x18, 0x21, 0x2B, 0x34, 0x3C, 0x43, 0x48, 0x4C,
    0x4F, 0x52, 0x55, 0x57, 0x59, 0x5B, 0x5D, 0x5F, 0x60, 0x61,
    0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
    0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D,
    0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA6, 0xA8, 0xAB, 0xAE,
    0xB1, 0xB5, 0xBA, 0xC1, 0xC9, 0xD2, 0xDC, 0xE7, 0xF3, 0xFF
};

/* The stage indices the firmware uses.  Steps 0..2 walk the three held
 * stages; step 3 holds the sustain, step 4 is the release and step 5
 * is the finished envelope. */
#define SLOPSIX_PITCH_EG_SUSTAIN 3u
#define SLOPSIX_PITCH_EG_RELEASE 4u
#define SLOPSIX_PITCH_EG_DONE    5u

/*
 * One pitch-EG update: the firmware's stage walk, in its 16-bit level
 * domain.
 *
 * A stage whose target equals the current level finishes immediately and
 * the next stage is selected on the following update.  Otherwise the
 * level moves by the stage's increment and finishes when its high byte
 * reaches the target's high byte; the level is then snapped to the
 * target.  That high-byte comparison is the firmware's own (it compares
 * the high bytes after the 16-bit add/subtract), and it is why the
 * engine keeps the unsigned level rather than a signed offset.  The
 * release stage is entered at step 4 and runs with the fourth rate/level
 * pair.
 */
void slopsix_pitch_eg_update(slopsix_pitch_eg *eg)
{
    uint8_t index;
    int32_t target;
    int32_t level;

    if (eg->step == SLOPSIX_PITCH_EG_SUSTAIN ||
        eg->step == SLOPSIX_PITCH_EG_DONE) {
        return;
    }

    /* Steps 4 and 5 use the fourth rate/level pair. */
    index = (eg->step > 3u) ? 3u : eg->step;
    target = eg->target[index];
    level = eg->level;

    if (level == target) {
        eg->step++;
        return;
    }

    if (level < target) {
        level += eg->increment[index];
        if ((level >> 8) >= (target >> 8)) {
            /* The high byte reached the target's: the stage is done. */
            level = target;
            eg->step++;
        }
    } else {
        level -= eg->increment[index];
        if (level < 0 || (level >> 8) <= (target >> 8)) {
            /* A negative result is the firmware's underflow finish; a
             * high byte at or below the target's is the normal one. */
            level = target;
            eg->step++;
        }
    }

    eg->level = level;
}

/*
 * The current contribution to the voice's log-frequency word, in the
 * engine's 4096-per-octave units.  The neutral level is zero and the
 * extremes are -16384 and +16256, i.e. about -4 and +3.97 octaves.
 */
uint8_t slopsix_pitch_eg_rate_value(uint8_t rate)
{
    if (rate > 99u) {
        rate = 99u;
    }
    return slopsix_pitch_eg_rate[rate];
}

int32_t slopsix_pitch_eg_offset(const slopsix_pitch_eg *eg)
{
    return eg->level - SLOPSIX_PITCH_EG_NEUTRAL_WORD;
}

/*
 * Load a patch's four rate/level pairs.  The firmware quantizes both at
 * patch activation and the final level is the initial level, so the
 * current level starts at the release level with the envelope at step 0.
 */
void slopsix_pitch_eg_init(slopsix_pitch_eg *eg, const uint8_t rates[4],
    const uint8_t levels[4])
{
    unsigned i;

    memset(eg, 0, sizeof(*eg));

    for (i = 0; i < 4u; i++) {
        uint8_t rate = rates[i] > 99u ? 99u : rates[i];
        uint8_t level = levels[i] > 99u ? 99u : levels[i];

        eg->increment[i] = slopsix_pitch_eg_rate[rate];
        eg->target[i] = (int32_t)slopsix_pitch_eg_level[level] << 7;
    }

    eg->level = eg->target[3];
    eg->step = 0u;
}

/*
 * Key-on: the envelope starts at the final (release) level and walks the
 * held stages from the first.  The cadence is not reset: the hardware's
 * toggle is global, so a note started between two ticks waits for the
 * next one.
 */
void slopsix_pitch_eg_note_on(slopsix_pitch_eg *eg)
{
    eg->level = eg->target[3];
    eg->step = 0u;
}

/*
 * Key-off: the release stage starts from the current level.  The firmware
 * sets the step to 4 whatever the previous step was.
 */
void slopsix_pitch_eg_note_off(slopsix_pitch_eg *eg)
{
    eg->step = SLOPSIX_PITCH_EG_RELEASE;
}
