/*
 * src/dx7s/slopsix_filter.c - the DX7S analog tail.
 *
 * The DX7S is DX7II-class hardware: a PCM54HP 16-bit DAC feeds a board
 * output stage that is not the Mk1's companded 12-bit one.  This file owns
 * the DX7S's own reconstruction corner and designs its generated prototype
 * through the shared filter core (src/slopsix_filter.c), exactly as the
 * Mk1's tail does with its prototype.  The core's math - the inverse
 * bilinear transform of the analog prototype, the corner-preserving rate
 * scaling and the 5th-order cascade - is signal-domain agnostic, so the
 * only personality-specific data is the prototype table and the corner.
 *
 * The signal domain.  The core consumes a DAC code and scales it to Q12
 * internally; the code is an int32 so the 16-bit DAC's full-scale code
 * (±32768) survives without a downshift.  Its output is Q12 of that same
 * 16-bit code, which is what slopsix_dx7s_output_scale expects when it
 * folds the gain out with a 28-bit shift (12 signal bits + 16 gain bits).
 *
 * Evidence grade: experimental placeholder.  The exact DX7S response is
 * not pinned by a capture or derived from the schematic yet.  The
 * service-manual overall circuit diagram (p. 40) shows a discrete
 * multi-section filter with the same nominal 3.3k/1.2k and
 * 0.0082/0.015/0.001/0.068 uF values as the Mk1 tail, so the 20 kHz
 * corner below is not supported by that sheet.  The prototype
 * (slopsix_dx7s_filter_prototype, generated into src/slopsix_tables.c only
 * for this personality) is the DX7 base fit rescaled to 20 kHz as a
 * placeholder.  A capture or a schematic-derived response replaces that
 * table and this corner, not the core and not the DX7's tail.
 *
 * This translation unit is initialization-only, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* The DX7S tail's -3 dB corner, in Hz: the corner the shared rate scaling
 * keeps in place.  It matches the generated prototype's own -3 dB point,
 * so the anchor rate's design is the prototype unchanged (kappa = 1).
 * This is a placeholder: the service manual's overall circuit diagram shows
 * Mk1-class component values, and no DX7S capture has pinned a 20 kHz
 * corner. */
#define SLOPSIX_DX7S_FILTER_CORNER_HZ 20000

void slopsix_dx7s_filter_init(slopsix_filter *filter, uint32_t sample_rate)
{
    slopsix_filter_init_with(filter, slopsix_dx7s_filter_prototype,
        SLOPSIX_DX7S_FILTER_CORNER_HZ, sample_rate);
}
