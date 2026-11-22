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
 * The DX7's tail is the original 5th-order cascade - a first-order section
 * and two biquads - whose prototype is the candidate fit of the VDX7
 * reference response (algorithm page §8.1).  The prototype is a *magnitude
 * fit*, not a circuit derivation: the hardware's exact response, phase and
 * stopband are still open (research OQ-22).
 *
 * The DX7S's tail is the experimental section-descriptor design: the
 * aperture shelf first, then a 24 dB/oct Butterworth low-pass at 21 kHz
 * (two biquads, Q 0.5412 and Q 1.3066).  The shelf leads so its ~2.4 dB
 * treble cut gives headroom before the resonating Q 1.3066 stage in the
 * fixed-point cascade.  The aperture shelf is a high-shelf cut fitted to
 * the DAC's sinc droop, not an inverse-sinc boost: both personalities are
 * non-oversampled ~49.1 kHz DACs.
 *
 * The design at the host's rate.
 *
 * The hardware's filter is analog: its corner is fixed in Hz.  The DX7's
 * prototype is a *digital* design at the engine's anchor rate (49,096 Hz),
 * so its digital coefficients cannot be reused at another host rate.  The
 * engine therefore keeps the analog prototype - the sections' coefficients
 * mapped through the inverse bilinear transform - and derives the digital
 * design for the host's rate at context creation.
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
 * so that the response's -3 dB point stays at the DX7 tail's corner
 * (SLOPSIX_FILTER_CORNER_HZ) whatever the host's rate.  The scale is a
 * constant, so the response's shape is warped with the rate; the corner -
 * the most audible feature of the curve - stays put.
 *
 * The DX7S describes its tail as *section descriptors* (kind, corner, Q,
 * shelf gain; see the generated tables), and the digital coefficients are
 * designed at the host's rate at context creation.  A plain bilinear
 * transform would cramp the top octave towards Nyquist (the DX7S's 21 kHz
 * corner in particular); the low-pass sections use Vicanek's matched design
 * (Martin Vicanek, "Matched Second Order Digital Filters", 2016, section
 * 4.1) instead: the poles are mapped by impulse invariance and the
 * numerator is fixed by unity DC gain and a match at the corner.  That
 * keeps the response close to the analog prototype right up to Nyquist at
 * 44.1/48 kHz, with no oversampling.  The shelf is an RBJ high-shelf
 * designed at the host's rate, and its corner is clamped to 0.45 fs so the
 * poles stay inside the unit circle at low host rates.
 *
 * The arithmetic is integer-only: the transcendentals the designs need
 * (tan, sin/cos, exp, sqrt) are init-only fixed-point routines below, never
 * on the sample path.  The coefficients are Q30, the signal is the DAC's
 * code in Q12 and the multiply-accumulate is 64-bit with round-to-nearest.
 * The design runs once per context (and once per sample-rate change).
 *
 * This translation unit is on the per-sample render path and is compiled
 * speed-first (-O3); the rest of the library is compiled size-first.
 */
#include <string.h>

#include "slopsix_internal.h"

/* The process path unrolls the cascade: one first-order section and two
 * biquads for the DX7, three biquads for the DX7S. */
typedef char slopsix_filter_cascade_check
    [SLOPSIX_FILTER_SECTIONS == 3 ? 1 : -1];

/* The DX7 prototype's sections and their orders: the rows of the generated
 * table are b0, b1, b2, a1, a2. */
static const uint8_t slopsix_filter_order[SLOPSIX_FILTER_SECTIONS] = {
    1u, 2u, 2u
};

/* The DX7 tail's -3 dB corner, in Hz: the corner the rate scaling keeps in
 * place.  It matches the generated prototype's own -3 dB point, so the
 * anchor rate's design is the prototype unchanged (kappa = 1).  This is the
 * base research fit's measured 17.02 kHz corner. */
#define SLOPSIX_FILTER_CORNER_HZ 17020

/* The design's fixed point: Q24 intermediates, Q30 results. */
#define SLOPSIX_FILTER_Q24 24
#define SLOPSIX_FILTER_ONE24 ((int64_t)1 << SLOPSIX_FILTER_Q24)
#define SLOPSIX_FILTER_Q30 30

/* The section's output limit: the accumulator's saturation, in Q12 of a
 * DAC code.  The largest legitimate value is a few times the code's full
 * scale - 8.4e6 for the Mk1's 12-bit codes, 134e6 for the DX7S's 16-bit
 * ones - so the limit is above anything the filter can reach; it only
 * keeps a runaway state from becoming an overflow. */
#define SLOPSIX_FILTER_LIMIT 536870912

/* pi, pi/2 and 2 pi in Q24. */
#define SLOPSIX_FILTER_PI_Q24 52707178LL
#define SLOPSIX_FILTER_HALF_PI_Q24 26353589LL
#define SLOPSIX_FILTER_TWO_PI_Q24 105414357LL

/* (ln 10 / 40) in Q24: the shelf gain's conversion from dB/40 to the
 * natural-exponent argument of the amplitude factor. */
#define SLOPSIX_FILTER_LN10_40_Q24 966057LL

/* The corner clamp: a section's corner never exceeds this fraction of the
 * host's rate, so its poles stay inside the unit circle at every rate. */
#define SLOPSIX_FILTER_MAX_CORNER_NUM 45u
#define SLOPSIX_FILTER_MAX_CORNER_DEN 100u

/* sin(x) for x in [0, pi/2] (Q24) by its Taylor series through x^9.  The
 * truncation leaves about 3.5e-6 of a unit at pi/2, far below the design's
 * resolution. */
static int64_t slopsix_filter_sin_series_q24(int64_t x)
{
    int64_t x2 = (x * x) >> SLOPSIX_FILTER_Q24;
    int64_t term = x;
    int64_t sum = x;

    term = (term * x2) >> SLOPSIX_FILTER_Q24;
    sum -= term / 6;
    term = (term * x2) >> SLOPSIX_FILTER_Q24;
    sum += term / 120;
    term = (term * x2) >> SLOPSIX_FILTER_Q24;
    sum -= term / 5040;
    term = (term * x2) >> SLOPSIX_FILTER_Q24;
    sum += term / 362880;

    return sum;
}

/* The sine and the cosine of an angle in Q24 radians, for any angle.  The
 * angle is reduced to a quadrant and a remainder in [0, pi/2]; the series
 * only has to cover that range. */
static void slopsix_filter_sincos_q24(int64_t x, int64_t *sin_out,
    int64_t *cos_out)
{
    int64_t quadrant;
    int64_t rem;
    int64_t sr;
    int64_t cr;

    x %= SLOPSIX_FILTER_TWO_PI_Q24;
    if (x < 0) {
        x += SLOPSIX_FILTER_TWO_PI_Q24;
    }
    quadrant = x / SLOPSIX_FILTER_HALF_PI_Q24;
    rem = x - quadrant * SLOPSIX_FILTER_HALF_PI_Q24;

    sr = slopsix_filter_sin_series_q24(rem);
    cr = slopsix_filter_sin_series_q24(SLOPSIX_FILTER_HALF_PI_Q24 - rem);

    switch (quadrant) {
    case 0:  *sin_out = sr;  *cos_out = cr;  break;
    case 1:  *sin_out = cr;  *cos_out = -sr; break;
    case 2:  *sin_out = -sr; *cos_out = -cr; break;
    default: *sin_out = -cr; *cos_out = sr;  break;
    }
}

/* exp(-x) for x >= 0 (Q24), by its Taylor series through x^16.  At the
 * largest argument the design uses (a little above pi) the truncation is
 * below 3e-5 of a unit. */
static int64_t slopsix_filter_exp_neg_q24(int64_t x)
{
    int64_t term = SLOPSIX_FILTER_ONE24;
    int64_t sum = SLOPSIX_FILTER_ONE24;
    int k;

    if (x < 0) {
        x = 0;
    }
    for (k = 1; k <= 16; k++) {
        term = (term * x) / (SLOPSIX_FILTER_ONE24 * k);
        sum += term;
    }

    return (SLOPSIX_FILTER_ONE24 * SLOPSIX_FILTER_ONE24) / sum;
}

/* The integer square root of a 64-bit value. */
static uint64_t slopsix_filter_isqrt64(uint64_t n)
{
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }

    return r;
}

/* sqrt(v) for a Q24 value v >= 0. */
static int64_t slopsix_filter_sqrt_q24(int64_t v)
{
    if (v <= 0) {
        return 0;
    }

    return (int64_t)slopsix_filter_isqrt64((uint64_t)v
        << SLOPSIX_FILTER_Q24);
}

/* The Q30 value, clamped into the int32 word the section keeps. */
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

/* The rate scaling: the prototype's frequency axis is scaled by kappa so
 * that the response's -3 dB point stays at the DX7 tail's corner.
 *
 *   kappa = (fs0 / fs) * tan (pi fc / fs0) / tan (pi fc / fs)
 *
 * The two tangents are computed by the same routine, so at the anchor
 * rate they are equal and kappa is exactly one: the anchor rate's design
 * is the prototype unchanged. */
static int64_t slopsix_filter_kappa_q30(uint32_t corner_hz,
    uint32_t sample_rate)
{
    /* pi * the corner, in Q24. */
    int64_t corner_q24 = (int64_t)corner_hz * SLOPSIX_FILTER_PI_Q24;
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

/* The prototype section's design at the host's rate.
 *
 * The prototype's coefficients are mapped through the inverse bilinear
 * transform at the anchor rate, the prototype's frequency axis is scaled
 * by kappa, and the result is mapped forward through the bilinear
 * transform at the host's rate.  The section's gain is folded into the
 * numerator, so the section's DC gain is one. */
static void slopsix_filter_design_prototype(unsigned order,
    const int32_t *proto, int64_t kappa_q24, int64_t r_q24,
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

/* The corner in millihertz, clamped to 0.45 of the host's rate. */
static int64_t slopsix_filter_corner_millihz(int64_t freq_millihz,
    uint32_t sample_rate)
{
    int64_t limit = (int64_t)sample_rate * 1000
        * SLOPSIX_FILTER_MAX_CORNER_NUM / SLOPSIX_FILTER_MAX_CORNER_DEN;

    if (freq_millihz > limit) {
        return limit;
    }

    return freq_millihz;
}

/* The Q24 angular corner, 2 pi f0 / fs. */
static int64_t slopsix_filter_omega_q24(int64_t freq_millihz,
    uint32_t sample_rate)
{
    return (2 * SLOPSIX_FILTER_PI_Q24 * freq_millihz) /
        ((int64_t)1000 * (int64_t)sample_rate);
}

/* Vicanek's matched low-pass at the host's rate (Q24 coefficients, DC gain
 * one).  The poles come from impulse invariance, the single zero and the
 * two numerator terms from unity DC gain and a magnitude match at the
 * corner; b2 is zero so the response does not plunge to zero at Nyquist. */
static void slopsix_filter_design_lowpass(int64_t freq_millihz,
    int64_t q_tenk, uint32_t sample_rate, int64_t coeff_q24[5])
{
    int64_t f0 = slopsix_filter_corner_millihz(freq_millihz, sample_rate);
    int64_t w0 = slopsix_filter_omega_q24(f0, sample_rate);
    int64_t q_q24;
    int64_t q_sq;
    int64_t root;
    int64_t angle;
    int64_t sin_angle;
    int64_t cos_angle;
    int64_t pole;
    int64_t a1;
    int64_t a2;
    int64_t sin_half;
    int64_t cos_half;
    int64_t phi1;
    int64_t phi0;
    int64_t phi2;
    int64_t a0_sq;
    int64_t a1_sq;
    int64_t a2_lin;
    int64_t atot;
    int64_t q_q24b;
    int64_t q2;
    int64_t r1;
    int64_t b1_sq;
    int64_t root_b0;
    int64_t root_b1;

    /* q = 1/(2Q), in Q24; Q is stored as Q*10000. */
    if (q_tenk < 5000) {
        q_tenk = 5000;
    }
    q_q24 = (SLOPSIX_FILTER_ONE24 * 10000) / (2 * q_tenk);

    /* The impulse-invariance poles.  q <= 1 for every Q >= 1/2, so the
     * cosine form applies: a1 = -2 e^{-q w0} cos(w0 sqrt(1-q^2)), and
     * a2 = e^{-2 q w0}. */
    q_sq = (q_q24 * q_q24) >> SLOPSIX_FILTER_Q24;
    root = slopsix_filter_sqrt_q24(SLOPSIX_FILTER_ONE24 - q_sq);
    angle = (w0 * root) >> SLOPSIX_FILTER_Q24;
    slopsix_filter_sincos_q24(angle, &sin_angle, &cos_angle);
    (void)sin_angle;
    pole = slopsix_filter_exp_neg_q24((q_q24 * w0)
        >> SLOPSIX_FILTER_Q24);
    a1 = -2 * ((pole * cos_angle) >> SLOPSIX_FILTER_Q24);
    a2 = (pole * pole) >> SLOPSIX_FILTER_Q24;

    /* The phi terms at the digital corner: phi1 = sin^2(w0/2). */
    slopsix_filter_sincos_q24(w0 / 2, &sin_half, &cos_half);
    (void)cos_half;
    phi1 = (sin_half * sin_half) >> SLOPSIX_FILTER_Q24;
    phi0 = SLOPSIX_FILTER_ONE24 - phi1;
    phi2 = (4 * phi0 * phi1) >> SLOPSIX_FILTER_Q24;

    /* A0, A1, A2 from the denominator coefficients. */
    {
        int64_t t = SLOPSIX_FILTER_ONE24 + a1 + a2;
        a0_sq = (t * t) >> SLOPSIX_FILTER_Q24;
        t = SLOPSIX_FILTER_ONE24 - a1 + a2;
        a1_sq = (t * t) >> SLOPSIX_FILTER_Q24;
        a2_lin = -4 * a2;
    }
    atot = ((a0_sq * phi0) >> SLOPSIX_FILTER_Q24)
        + ((a1_sq * phi1) >> SLOPSIX_FILTER_Q24)
        + ((a2_lin * phi2) >> SLOPSIX_FILTER_Q24);

    /* R1 = |H|^2 at the corner times the denominator there; with b2 = 0 and
     * B0 = A0, solve for B1 and then the numerator terms. */
    q_q24b = (q_tenk * SLOPSIX_FILTER_ONE24) / 10000;
    q2 = (q_q24b * q_q24b) >> SLOPSIX_FILTER_Q24;
    r1 = (atot * q2) >> SLOPSIX_FILTER_Q24;
    b1_sq = ((r1 - ((a0_sq * phi0) >> SLOPSIX_FILTER_Q24))
        * SLOPSIX_FILTER_ONE24) / phi1;
    if (b1_sq < 0) {
        b1_sq = 0;
    }

    root_b0 = slopsix_filter_sqrt_q24(a0_sq);
    root_b1 = slopsix_filter_sqrt_q24(b1_sq);

    coeff_q24[0] = (root_b0 + root_b1) / 2;
    coeff_q24[1] = (root_b0 - root_b1) / 2;
    coeff_q24[2] = 0;
    coeff_q24[3] = a1;
    coeff_q24[4] = a2;
}

/* An RBJ high-shelf at the host's rate (Q24 coefficients normalized by a0),
 * used for the aperture droop. */
static void slopsix_filter_design_shelf(int64_t freq_millihz,
    int64_t q_tenk, int64_t gain_millidb, uint32_t sample_rate,
    int64_t coeff_q24[5])
{
    int64_t f0 = slopsix_filter_corner_millihz(freq_millihz, sample_rate);
    int64_t w0 = slopsix_filter_omega_q24(f0, sample_rate);
    int64_t sin_w;
    int64_t cos_w;
    int64_t arg;
    int64_t amp;
    int64_t root_amp;
    int64_t alpha;
    int64_t amp_p1;
    int64_t amp_m1;
    int64_t t1;
    int64_t t2;
    int64_t a0;

    if (q_tenk <= 0) {
        q_tenk = 5000;
    }

    slopsix_filter_sincos_q24(w0, &sin_w, &cos_w);

    /* amp = 10^(gain_db/40); the gain is negative for this cut. */
    arg = gain_millidb * SLOPSIX_FILTER_LN10_40_Q24 / 1000;
    if (arg < 0) {
        amp = slopsix_filter_exp_neg_q24(-arg);
    } else {
        amp = (SLOPSIX_FILTER_ONE24 * SLOPSIX_FILTER_ONE24)
            / slopsix_filter_exp_neg_q24(arg);
    }
    root_amp = slopsix_filter_sqrt_q24(amp);

    alpha = (sin_w * 10000) / (2 * q_tenk);
    amp_p1 = amp + SLOPSIX_FILTER_ONE24;
    amp_m1 = amp - SLOPSIX_FILTER_ONE24;
    t1 = (amp_m1 * cos_w) >> SLOPSIX_FILTER_Q24;
    t2 = 2 * ((root_amp * alpha) >> SLOPSIX_FILTER_Q24);

    coeff_q24[0] = (amp * (amp_p1 + t1 + t2)) >> SLOPSIX_FILTER_Q24;
    coeff_q24[1] = -2 * ((amp * (amp_m1
        + ((amp_p1 * cos_w) >> SLOPSIX_FILTER_Q24)))
        >> SLOPSIX_FILTER_Q24);
    coeff_q24[2] = (amp * (amp_p1 + t1 - t2)) >> SLOPSIX_FILTER_Q24;

    a0 = amp_p1 - t1 + t2;
    if (a0 <= 0) {
        a0 = 1;
    }
    /* Normalize the numerator by a0 and fold the denominator's own a0 out,
     * leaving 1 + a1 z^-1 + a2 z^-2. */
    coeff_q24[0] = (coeff_q24[0] * SLOPSIX_FILTER_ONE24) / a0;
    coeff_q24[1] = (coeff_q24[1] * SLOPSIX_FILTER_ONE24) / a0;
    coeff_q24[2] = (coeff_q24[2] * SLOPSIX_FILTER_ONE24) / a0;
    coeff_q24[3] = (2 * (amp_m1
        - ((amp_p1 * cos_w) >> SLOPSIX_FILTER_Q24)))
        * SLOPSIX_FILTER_ONE24 / a0;
    coeff_q24[4] = (amp_p1 - t1 - t2) * SLOPSIX_FILTER_ONE24 / a0;
}

/* Q24 to the int32 Q30 the section keeps. */
static int32_t slopsix_filter_to_q30(int64_t value_q24)
{
    return (int32_t)slopsix_filter_clamp_q30(
        value_q24 * ((int64_t)1 << (SLOPSIX_FILTER_Q30
            - SLOPSIX_FILTER_Q24)));
}

/* One section's design at the host's rate, from its descriptor. */
static void slopsix_filter_design(const int32_t *desc, uint32_t sample_rate,
    slopsix_filter_section *out)
{
    int64_t coeff[5];

    if (desc[0] == SLOPSIX_FILTER_IDENTITY) {
        out->b0 = (int32_t)((int64_t)1 << SLOPSIX_FILTER_Q30);
        out->b1 = 0;
        out->b2 = 0;
        out->a1 = 0;
        out->a2 = 0;
        return;
    }

    if (desc[0] == SLOPSIX_FILTER_HIGHSHELF) {
        slopsix_filter_design_shelf(desc[1], desc[2], desc[3], sample_rate,
            coeff);
    } else {
        slopsix_filter_design_lowpass(desc[1], desc[2], sample_rate, coeff);
    }

    out->b0 = slopsix_filter_to_q30(coeff[0]);
    out->b1 = slopsix_filter_to_q30(coeff[1]);
    out->b2 = slopsix_filter_to_q30(coeff[2]);
    out->a1 = slopsix_filter_to_q30(coeff[3]);
    out->a2 = slopsix_filter_to_q30(coeff[4]);
}

/* One section's step: the direct form I of the section's difference
 * equation, in Q12 with the coefficients in Q30.  The accumulator is
 * 64-bit and the result is rounded to nearest, half away from zero. */
static int32_t slopsix_filter_round(int64_t acc)
{
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

    return y;
}

static int32_t slopsix_filter_step(slopsix_filter_section *section,
                                    int32_t x)
{
    int64_t acc = (int64_t)section->b0 * (int64_t)x +
                  (int64_t)section->b1 * (int64_t)section->x1 +
                  (int64_t)section->b2 * (int64_t)section->x2 -
                  (int64_t)section->a1 * (int64_t)section->y1 -
                  (int64_t)section->a2 * (int64_t)section->y2;
    int32_t y = slopsix_filter_round(acc);

    section->x2 = section->x1;
    section->x1 = x;
    section->y2 = section->y1;
    section->y1 = y;

    return y;
}

/* The filter's design at a host rate, and its state cleared.
 *
 * The prototype is the personality's section-descriptor table: each row is
 * a kind, a corner in millihertz, a Q in ten-thousandths and a shelf gain
 * in millibels.  The digital coefficients are designed at the host's rate;
 * there is no anchor rate and no rate scaling, so the tail is stable and
 * cramp-free at 44.1/48 kHz as well as at the hardware's 49.096 kHz. */
void slopsix_filter_init_with(slopsix_filter *filter,
    const int32_t desc[SLOPSIX_FILTER_SECTIONS][4],
    uint32_t sample_rate)
{
    unsigned i;

    memset(filter, 0, sizeof(*filter));

    if (sample_rate == 0u) {
        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            filter->section[i].b0 = (int32_t)((int64_t)1 << 30);
        }
        return;
    }

    for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
        slopsix_filter_design(desc[i], sample_rate, &filter->section[i]);
    }
}

/* The DX7 tail's design from its anchor-rate prototype and -3 dB corner.
 *
 * A host rate that cannot carry the corner gets a transparent filter: the
 * prototype's response is flat below the corner, and the corner itself is
 * at or above the host's Nyquist.  A rate above the cadences' cap is
 * clamped like the cadence gate clamps its rate: the bilinear transform's
 * Q24 products stay inside 64 bits only up to there, and above it the
 * design overflows instead of degenerating. */
void slopsix_filter_init_prototype(slopsix_filter *filter,
    const int32_t prototype[SLOPSIX_FILTER_SECTIONS][5],
    uint32_t corner_hz, uint32_t sample_rate)
{
    unsigned i;

    memset(filter, 0, sizeof(*filter));

    if (sample_rate == 0u) {
        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            filter->section[i].b0 = (int32_t)((int64_t)1 << 30);
        }
        return;
    }

    if (sample_rate > SLOPSIX_DEFAULT_SAMPLE_RATE * 8u) {
        sample_rate = SLOPSIX_DEFAULT_SAMPLE_RATE * 8u;
    }

    /* Below twice the corner the corner sits at or above the host's
     * Nyquist: the prototype is flat below its corner, so the filter is
     * transparent there. */
    if (sample_rate <= 2u * corner_hz) {
        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            filter->section[i].b0 = (int32_t)((int64_t)1 << 30);
        }
        return;
    }

    {
        int64_t kappa_q24 =
            slopsix_filter_kappa_q30(corner_hz, sample_rate) >> 6;
        int64_t r_q24 = ((int64_t)sample_rate << SLOPSIX_FILTER_Q24) /
                        (int64_t)SLOPSIX_DEFAULT_SAMPLE_RATE;

        for (i = 0; i < SLOPSIX_FILTER_SECTIONS; i++) {
            slopsix_filter_design_prototype(slopsix_filter_order[i],
                prototype[i], kappa_q24, r_q24, &filter->section[i]);
        }
    }
}

/* The DX7 personality's tail: its anchor-rate prototype through the shared
 * transform and rate scaling. */
void slopsix_filter_init(slopsix_filter *filter, uint32_t sample_rate)
{
    slopsix_filter_init_prototype(filter, slopsix_filter_prototype,
        SLOPSIX_FILTER_CORNER_HZ, sample_rate);
}

/* One sample through the filter: the DAC's code in, the analog stage's
 * output out, in Q12 of a DAC code. */
int32_t slopsix_filter_process(slopsix_filter *filter, int32_t code)
{
    /* The code in Q12: the shift is a multiplication, because a left
     * shift of a negative value is not defined. */
    int32_t x = code * (1 << SLOPSIX_FILTER_Q12);

    x = slopsix_filter_step(&filter->section[0], x);
    x = slopsix_filter_step(&filter->section[1], x);
    x = slopsix_filter_step(&filter->section[2], x);

    return x;
}
