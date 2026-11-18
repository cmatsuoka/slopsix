/*
 * slopsix_filter.c - The analog tail's reconstruction filter.
 *
 * The DX7's board ends in an analog stage: the 12-bit DAC's output is
 * held by two alternating sample-and-hold circuits, summed, and low-passed
 * by a multi-section analog filter before the volume control (the service
 * manual's filter sheet around IC54, distilled on the dx7-output research
 * page, OU-04/OQ-22).  The engine models the filter; the sample-and-hold
 * reconstruction stays out (research OQ-23).
 *
 * The filter is a 5th-order cascade - a first-order section and two
 * biquads - whose prototype is the candidate fit of the VDX7 reference
 * response (algorithm page §8.1).  The prototype is a *magnitude fit*, not
 * a circuit derivation: the hardware's exact response, phase and stopband
 * are still open (research OQ-22).  The engine implements it as it
 * stands.
 *
 * The filter is a *hardware* stage of the DX7 personality, so it lives
 * here, in the DX7's own translation unit, and not in the shared output
 * stages: a later personality brings its own tail.
 *
 * The rate.
 *
 * The hardware's filter is analog: its corner is fixed in Hz.  The
 * prototype above is a *digital* design at the engine's anchor rate
 * (49,096 Hz), so its digital coefficients cannot be reused at another
 * host rate.  The engine therefore keeps the analog prototype - the
 * sections' coefficients mapped through the inverse bilinear transform -
 * and derives the digital design for the host's rate at context creation.
 *
 * The bilinear substitution is a Moebius map in both directions, so the
 * transform is exact in rational arithmetic and needs no roots:
 *
 *   inverse (at the anchor rate)   z^-1 = (1 - x) / (1 + x),  x = s / (2 fs0)
 *   forward  (at the host's rate)  x    = r (1 - z^-1) / (1 + z^-1), r = fs / fs0
 *
 * for one section
 *
 *   D0 = 1 + a1 + a2,  D1 = 2 (1 - a2),  D2 = 1 - a1 + a2
 *   A0 = D0 + D1 r + D2 r^2
 *   A1 = 2 D0 - 2 D2 r^2
 *   A2 = D0 - D1 r + D2 r^2
 *   b  = the numerator, mapped the same way, divided by A0
 *
 * That transform alone keeps the *analog prototype* and samples it at the
 * host's rate, which moves the whole response when the rate moves: at
 * 44.1 kHz the top end falls away far too fast and at 96 kHz it does not
 * fall away at all.  The engine therefore also scales the prototype's
 * frequency axis by a constant
 *
 *   kappa = (fs0 / fs) * tan (pi fc / fs0) / tan (pi fc / fs)
 *
 * so that the response's -3 dB point stays at the reference's corner
 * (17.02 kHz) whatever the host's rate.  The scale is a constant, so the
 * response's shape is warped with the rate; the corner - the most audible
 * feature of the curve - stays put.
 *
 * The arithmetic is integer-only: the coefficients are Q30, the signal is
 * the DAC's code in Q12 (the analog stage's output is continuous, so the
 * code's integer grid is too coarse) and the multiply-accumulate is
 * 64-bit with round-to-nearest.  The transform runs once per context
 * (and once per sample-rate change), never on the sample path.
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O2); the rest of the library is compiled size-first.
 */
#include <string.h>

#include "slopsix_internal.h"

/* The prototype's sections and their orders: the rows of the generated
 * table are b0, b1, b2, a1, a2. */
static const uint8_t slopsix_filter_order[SLOPSIX_FILTER_SECTIONS] = {
    1u, 2u, 2u
};

/* The reference's -3 dB frequency, in Hz: the corner the rate scaling
 * keeps in place (the research page's response table: -3.00 dB at
 * 17.02 kHz).  It is the anchor rate's -3 dB point of the candidate fit,
 * so the anchor rate's design is this prototype unchanged (kappa = 1). */
#define SLOPSIX_FILTER_CORNER_HZ 17020

/* The lowest host rate that can carry the corner: below twice the corner
 * the corner would sit at or above the host's Nyquist, and the reference's
 * response is flat up to the corner, so the filter is then transparent.
 * The reference's response table: -0.24 dB at 16 kHz, so the curve is
 * flat well below the corner. */
#define SLOPSIX_FILTER_MIN_RATE (2u * SLOPSIX_FILTER_CORNER_HZ)

/* The transform's fixed point: Q24 intermediates, Q30 results.  The
 * prototype's Q30 values are scaled down by six bits and the results are
 * scaled back up, which costs 6e-8 of a unit - far below the response's
 * own accuracy. */
#define SLOPSIX_FILTER_Q24 24
#define SLOPSIX_FILTER_ONE24 ((int64_t)1 << SLOPSIX_FILTER_Q24)

/* The section's output limit: the accumulator's saturation, in Q12 of a
 * DAC code.  The largest legitimate value is a few times the code's full
 * scale (8.4e6), so the limit is far above anything the filter can
 * reach; it only keeps a runaway state from becoming an overflow. */
#define SLOPSIX_FILTER_LIMIT 536870912

/* pi in Q24. */
#define SLOPSIX_FILTER_PI_Q24 52707178LL

/* The tangent, by the [4/4] Pade approximation
 *
 *   tan(x) ~ x (945 - 105 x^2 + x^4) / (945 - 420 x^2 + 15 x^4)
 *
 * The approximation's pole sits at x = 1.5707, just below pi/2, so the
 * argument is clamped below it.  Over the range the corner's placement
 * needs (0 .. 1.55 rad) the approximation is within about 0.2%, and the
 * placement tolerates that: a 0.2% corner error is 34 Hz at 17 kHz.
 *
 * `x` is in Q24 and the result is in Q24. */
static int64_t slopsix_filter_tan_q24(int64_t x_q24)
{
    int64_t x2;
    int64_t x4;
    int64_t num;
    int64_t den;
    int64_t ratio;

    /* The largest argument the approximation is used at: 1.55 rad. */
    if (x_q24 > 26009139LL) {
        x_q24 = 26009139LL;
    }

    x2 = (x_q24 * x_q24) >> SLOPSIX_FILTER_Q24;
    x4 = (x2 * x2) >> SLOPSIX_FILTER_Q24;

    /* The numerator's and the denominator's polynomials, with the
     * coefficients 105/945 = 1/9, 1/945, 420/945 = 4/9 and 15/945
     * written as shifts and multiplications. */
    num = SLOPSIX_FILTER_ONE24 - ((x2 + 4u) / 9u) + ((x4 * 1088u) >> 20);
    den = SLOPSIX_FILTER_ONE24 - ((x2 * 4u + 4u) / 9u) +
          ((x4 * 16660u) >> 20);

    if (den <= 0) {
        /* The clamp above keeps the denominator positive; the guard is
         * here because the polynomial is evaluated in fixed point. */
        den = 1;
    }

    ratio = (num << SLOPSIX_FILTER_Q24) / den;

    return (x_q24 * ratio) >> SLOPSIX_FILTER_Q24;
}

/* The ratio of two Q24 values as a Q30 value.
 *
 * The quotient is taken first and the fraction follows, so that neither
 * the quotient's scaling nor the remainder's product can overflow: the
 * remainder is smaller than the divisor, and the divisor stays far below
 * the point where the remainder's product would overflow a 64-bit word.
 *
 * The scalings after the division are multiplications, not shifts: the
 * remainder and the divided fraction are signed whenever the numerator is
 * (a section's mapped pole coefficient is negative at every rate), and a
 * left shift of a negative value is not defined. */
static int64_t slopsix_filter_ratio_q30(int64_t num, int64_t den)
{
    int64_t whole = num / den;
    int64_t rem = num - whole * den;

    return whole * ((int64_t)1 << 30) +
           (rem * SLOPSIX_FILTER_ONE24) / den * 64;
}

/* The Q30 value, clamped into the int32 word the section keeps.  The
 * clamp only bites at the extremes of the rate range, where a
 * coefficient reaches two (a pole at z = 1). */
static int64_t slopsix_filter_clamp_q30(int64_t value)
{
    if (value > 2147483647LL) {
        return 2147483647LL;
    }
    if (value < -2147483648LL) {
        return -2147483648LL;
    }
    return value;
}

/* The rate scaling: the prototype's frequency axis is scaled by kappa so
 * that the response's -3 dB point stays at the reference's corner.
 *
 *   kappa = (fs0 / fs) * tan (pi fc / fs0) / tan (pi fc / fs)
 *
 * The two tangents are computed by the same routine, so at the anchor
 * rate they are equal and kappa is exactly one: the anchor rate's design
 * is the prototype unchanged. */
static int64_t slopsix_filter_kappa_q30(uint32_t sample_rate)
{
    /* pi * the corner, in Q24. */
    int64_t corner_q24 = (int64_t)SLOPSIX_FILTER_CORNER_HZ *
                         SLOPSIX_FILTER_PI_Q24;
    int64_t anchor_tan;
    int64_t host_tan;
    int64_t ratio_q30;
    int64_t scale_q30;

    anchor_tan = slopsix_filter_tan_q24(
        corner_q24 / (int64_t)SLOPSIX_DEFAULT_SAMPLE_RATE);
    host_tan = slopsix_filter_tan_q24(corner_q24 / (int64_t)sample_rate);
    if (host_tan <= 0) {
        return ((int64_t)1 << 30);
    }

    /* The ratio of the two tangents, as a Q30 value: both tangents are
     * Q24, so the numerator is scaled up to Q30 before the division. */
    ratio_q30 = (anchor_tan << 30) / host_tan;
    scale_q30 = ((int64_t)SLOPSIX_DEFAULT_SAMPLE_RATE << 30) /
                (int64_t)sample_rate;

    return (scale_q30 * ratio_q30) >> 30;
}

/* The section's design at the host's rate.
 *
 * The prototype's coefficients are mapped through the inverse bilinear
 * transform at the anchor rate, the prototype's frequency axis is scaled
 * by kappa, and the result is mapped forward through the bilinear
 * transform at the host's rate.  The section's gain is folded into the
 * numerator, so the section's DC gain is one. */
static void slopsix_filter_design(unsigned order, const int32_t *proto,
                                   int64_t kappa_q24, int64_t r_q24,
                                   slopsix_filter_section *out)
{
    /* The prototype's coefficients, from Q30 to Q24.  The shift is a
     * floor division: a right shift on a negative value is not defined
     * by the standard. */
    int64_t b0 = slopsix_floor_div64(proto[0], 6u);
    int64_t b1 = slopsix_floor_div64(proto[1], 6u);
    int64_t b2 = slopsix_floor_div64(proto[2], 6u);
    int64_t a1 = slopsix_floor_div64(proto[3], 6u);
    int64_t a2 = slopsix_floor_div64(proto[4], 6u);
    int64_t n0;
    int64_t n1;
    int64_t n2;
    int64_t d0;
    int64_t d1;
    int64_t d2;
    int64_t kappa2_q24;
    int64_t a0;
    int64_t a1n;
    int64_t a2n;
    int64_t b0n;
    int64_t b1n;
    int64_t b2n;

    /* The analog numerator and denominator, in the anchor rate's x domain
     * (x = s / (2 fs0)). */
    if (order == 2u) {
        n0 = b0 + b1 + b2;
        n1 = 2 * (b0 - b2);
        n2 = b0 - b1 + b2;
        d0 = SLOPSIX_FILTER_ONE24 + a1 + a2;
        d1 = 2 * (SLOPSIX_FILTER_ONE24 - a2);
        d2 = SLOPSIX_FILTER_ONE24 - a1 + a2;
    } else {
        n0 = b0 + b1;
        n1 = b0 - b1;
        n2 = 0;
        d0 = SLOPSIX_FILTER_ONE24 + a1;
        d1 = SLOPSIX_FILTER_ONE24 - a1;
        d2 = 0;
    }

    /* The prototype's frequency axis, scaled by kappa: the polynomial's
     * x terms take kappa and kappa^2. */
    kappa2_q24 = (kappa_q24 * kappa_q24) >> SLOPSIX_FILTER_Q24;
    n1 = (n1 * kappa_q24) >> SLOPSIX_FILTER_Q24;
    n2 = (n2 * kappa2_q24) >> SLOPSIX_FILTER_Q24;
    d1 = (d1 * kappa_q24) >> SLOPSIX_FILTER_Q24;
    d2 = (d2 * kappa2_q24) >> SLOPSIX_FILTER_Q24;

    /* The forward bilinear transform at the host's rate. */
    if (order == 2u) {
        int64_t rr = (r_q24 * r_q24) >> SLOPSIX_FILTER_Q24;
        int64_t dr = (d1 * r_q24) >> SLOPSIX_FILTER_Q24;
        int64_t drr = (d2 * rr) >> SLOPSIX_FILTER_Q24;

        a0 = d0 + dr + drr;
        a1n = 2 * d0 - 2 * drr;
        a2n = d0 - dr + drr;
        b0n = n0 + ((n1 * r_q24) >> SLOPSIX_FILTER_Q24) +
              ((n2 * rr) >> SLOPSIX_FILTER_Q24);
        b1n = (2 * n0) - (2 * ((n2 * rr) >> SLOPSIX_FILTER_Q24));
        b2n = n0 - ((n1 * r_q24) >> SLOPSIX_FILTER_Q24) +
              ((n2 * rr) >> SLOPSIX_FILTER_Q24);
    } else {
        int64_t dr = (d1 * r_q24) >> SLOPSIX_FILTER_Q24;

        a0 = d0 + dr;
        a1n = d0 - dr;
        a2n = 0;
        b0n = n0 + ((n1 * r_q24) >> SLOPSIX_FILTER_Q24);
        b1n = n0 - ((n1 * r_q24) >> SLOPSIX_FILTER_Q24);
        b2n = 0;
    }

    if (a0 <= 0) {
        /* The design cannot degenerate: the denominator's leading term is
         * positive for every rate.  The guard keeps a division by zero
         * out of the render path. */
        a0 = 1;
    }

    out->b0 = (int32_t)slopsix_filter_clamp_q30(
        slopsix_filter_ratio_q30(b0n, a0));
    out->b1 = (int32_t)slopsix_filter_clamp_q30(
        slopsix_filter_ratio_q30(b1n, a0));
    out->b2 = (int32_t)slopsix_filter_clamp_q30(
        slopsix_filter_ratio_q30(b2n, a0));
    out->a1 = (int32_t)slopsix_filter_clamp_q30(
        slopsix_filter_ratio_q30(a1n, a0));
    out->a2 = (int32_t)slopsix_filter_clamp_q30(
        slopsix_filter_ratio_q30(a2n, a0));
}

/* One section's step: the direct form I of the section's difference
 * equation, in Q12 with the coefficients in Q30.  The accumulator is
 * 64-bit and the result is rounded to nearest, half away from zero.
 *
 * The accumulator's saturation: the signal is bounded (the DAC's code
 * and the filter's own passband overshoot), so the accumulator cannot
 * overflow and the result cannot leave the section's range.  The clamp
 * is the backstop that keeps a runaway state from turning into an
 * overflow - it never bites in normal operation, since the section's
 * largest legitimate value is a few times the code's full scale. */
static int32_t slopsix_filter_step(slopsix_filter_section *section,
                                    int32_t x)
{
    int64_t acc = (int64_t)section->b0 * (int64_t)x +
                  (int64_t)section->b1 * (int64_t)section->x1 +
                  (int64_t)section->b2 * (int64_t)section->x2 -
                  (int64_t)section->a1 * (int64_t)section->y1 -
                  (int64_t)section->a2 * (int64_t)section->y2;
    int64_t half = (int64_t)1 << 29;
    int32_t y;

    if (acc >= 0) {
        y = (int32_t)((acc + half) >> 30);
    } else {
        y = (int32_t)(-(((-acc) + half) >> 30));
    }

    if (y > SLOPSIX_FILTER_LIMIT) {
        y = SLOPSIX_FILTER_LIMIT;
    } else if (y < -SLOPSIX_FILTER_LIMIT) {
        y = -SLOPSIX_FILTER_LIMIT;
    }

    section->x2 = section->x1;
    section->x1 = x;
    section->y2 = section->y1;
    section->y1 = y;

    return y;
}

/* The filter's design at a host rate, and its state cleared.
 *
 * A host rate that cannot carry the corner gets a transparent filter: the
 * reference's response is flat below the corner, and the corner itself is
 * at or above the host's Nyquist.  A rate above the cadences' cap is
 * clamped like the cadence gate clamps its rate: the bilinear transform's
 * Q24 products stay inside 64 bits only up to there, and above it the
 * design overflows instead of degenerating. */
void slopsix_filter_init(slopsix_filter *filter, uint32_t sample_rate)
{
    unsigned i;

    memset(filter, 0, sizeof(*filter));

    if (sample_rate > SLOPSIX_DEFAULT_SAMPLE_RATE * 8u) {
        sample_rate = SLOPSIX_DEFAULT_SAMPLE_RATE * 8u;
    }

    if (sample_rate <= SLOPSIX_FILTER_MIN_RATE) {
        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            filter->section[i].b0 = (int32_t)((int64_t)1 << 30);
        }
        return;
    }

    {
        int64_t kappa_q24 = slopsix_filter_kappa_q30(sample_rate) >> 6;
        int64_t r_q24 = ((int64_t)sample_rate << SLOPSIX_FILTER_Q24) /
                        (int64_t)SLOPSIX_DEFAULT_SAMPLE_RATE;

        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            slopsix_filter_design(slopsix_filter_order[i],
                slopsix_filter_prototype[i], kappa_q24, r_q24,
                &filter->section[i]);
        }
    }
}

/* One sample through the filter: the DAC's code in, the analog stage's
 * output out, in Q12 of a DAC code. */
int32_t slopsix_filter_process(slopsix_filter *filter, int32_t code)
{
    /* The code in Q12: the shift is a multiplication, because a left
     * shift of a negative value is not defined. */
    int32_t x = code * (1 << SLOPSIX_FILTER_Q12);
    unsigned i;

    for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
        x = slopsix_filter_step(&filter->section[i], x);
    }

    return x;
}
