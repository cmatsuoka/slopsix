/*
 * src/dx7s/slopsix_filter.c - the DX7S analog tail.
 *
 * The DX7S is DX7II-class hardware: a PCM54HP 16-bit DAC feeds a board
 * output stage that is not the Mk1's companded 12-bit one.  This file owns
 * the DX7S's own reconstruction prototype and designs it through the
 * shared filter core (src/slopsix_filter.c), exactly as the Mk1's tail
 * does with its prototype.  The core's math - the inverse bilinear
 * transform of the analog prototype, the corner-preserving rate scaling
 * and the 5th-order cascade - is signal-domain agnostic, so the only
 * personality-specific data is this table.
 *
 * The signal domain.  The core consumes a DAC code and scales it to Q12
 * internally; the code is an int32 so the 16-bit DAC's full-scale code
 * (±32768) survives without a downshift.  Its output is Q12 of that same
 * 16-bit code, which is what slopsix_dx7s_output_scale expects when it
 * folds the gain out with a 28-bit shift (12 signal bits + 16 gain bits).
 *
 * Evidence grade: provisional prototype (D7S-13).  The DX7S output stage's
 * exact response is not pinned from a schematic or a capture yet, so the
 * table below is the Mk1's candidate fit (algorithm page §8.1) reused as
 * the DX7S tail's placeholder.  The separation matters even while the
 * numbers match: a later DX7S capture replaces this table, not the core
 * and not the DX7's tail.
 *
 * This translation unit is initialization-only, so it is compiled
 * size-first.
 */
#include "slopsix_internal.h"

/* The prototype's coefficients, Q30: the rows are b0, b1, b2, a1, a2 and
 * the sections are the first-order one and the two biquads.  The values
 * are the Mk1 candidate fit until the DX7S tail is measured (D7S-13). */
static const int32_t slopsix_dx7s_filter_prototype[SLOPSIX_FILTER_SECTIONS][5] = {
    /* H1 first order */ { 535498195, 535498195, 0, -2745435, 0 },
    /* H2 biquad */ { 610876461, 1221752922, 610876461, 762572659, 607191361 },
    /* H3 biquad */ { 953291420, 1906582839, 953291420, 1794846297, 944577557 },
};

void slopsix_dx7s_filter_init(slopsix_filter *filter, uint32_t sample_rate)
{
    slopsix_filter_init_with(filter, slopsix_dx7s_filter_prototype,
        sample_rate);
}
