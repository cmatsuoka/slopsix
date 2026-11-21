/*
 * src/dx7s/slopsix_filter.c - the DX7S analog tail.
 *
 * The DX7S is DX7II-class hardware: a PCM54HP 16-bit DAC feeds a board
 * output stage that is not the Mk1's companded 12-bit one.  This file owns
 * the DX7S's own section descriptors and designs them through the shared
 * filter core (src/slopsix_filter.c), exactly as the Mk1's tail does.  The
 * core's math - the Vicanek-matched low-pass, the RBJ high-shelf and the
 * biquad cascade - is signal-domain agnostic, so the only
 * personality-specific data is the descriptor table.
 *
 * The signal domain.  The core consumes a DAC code and scales it to Q12
 * internally; the code is an int32 so the 16-bit DAC's full-scale code
 * (±32768) survives without a downshift.  Its output is Q12 of that same
 * 16-bit code, which is what slopsix_dx7s_output_scale expects when it
 * folds the gain out with a 28-bit shift (12 signal bits + 16 gain bits).
 *
 * EXPERIMENTAL: the tail is the shared zero-order-hold aperture shelf
 * first, then a 4th-order Butterworth low-pass at 21 kHz (two biquads,
 * Q 0.5412 and Q 1.3066).  The shelf leads the cascade so its treble cut
 * gives headroom before the resonating Q 1.3066 stage.  This deliberately
 * departs from the VDX7-derived fit and from the service manual's
 * Mk1-class component values; it is an unverified model, not a circuit
 * derivation.  The descriptors (slopsix_dx7s_filter_desc) are generated
 * into src/slopsix_tables.c only for this personality.  A capture or a
 * schematic-derived response replaces that table, not the core and not the
 * DX7's tail.
 *
 * This translation unit is initialization-only, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

void slopsix_dx7s_filter_init(slopsix_filter *filter, uint32_t sample_rate)
{
    slopsix_filter_init_with(filter, slopsix_dx7s_filter_desc, sample_rate);
}
