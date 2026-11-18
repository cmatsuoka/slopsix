#!/usr/bin/env python3
"""Generate the analog tail's reconstruction filter prototype.

The DX7's output path ends in an analog stage: the 12-bit DAC's output is
held by two alternating sample-and-hold circuits, summed, and low-passed by
a multi-section analog filter before the volume control.  The engine models
the filter (the sample-and-hold reconstruction stays out - research OQ-23).

The filter is modelled as a 5th-order cascade: a first-order section
followed by two biquads, all of them with their zeros at z = -1 (the
bilinear low-pass zero).  The prototype below is the candidate fit distilled
on /projects/slopsix/research/dx7-output - a magnitude fit of the VDX7
reference response over 0-20 kHz, not a circuit derivation (research
OQ-22).  The engine implements it as it stands.

The prototype is the design at the engine's anchor rate (49,096 Hz).  The
hardware's filter is analog, so its corner is fixed in Hz: the engine keeps
the analog prototype and derives the digital coefficients for the host's
rate at context creation.  The transform is the bilinear substitution in
both directions (a Moebius map, so no roots are involved):

    inverse (at the anchor rate)   z^-1 = (1 - x) / (1 + x),  x = s / (2 fs0)
    forward  (at the host rate)    x    = r (1 - z^-1) / (1 + z^-1), r = fs / fs0

which for one section reads

    D0 = 1 + a1 + a2,  D1 = 2 (1 - a2),  D2 = 1 - a1 + a2
    A0 = D0 + D1 r + D2 r^2
    A1 = 2 D0 - 2 D2 r^2
    A2 = D0 - D1 r + D2 r^2
    b  = the numerator, mapped the same way, divided by A0

That transform keeps the analog prototype and samples it at the host's
rate, which moves the whole response when the rate moves.  The engine also
scales the prototype's frequency axis by a constant

    kappa = (fs0 / fs) * tan (pi fc / fs0) / tan (pi fc / fs)

so that the response's -3 dB point stays at the reference's corner
(17.02 kHz) whatever the host's rate.

The tool checks the prototype's response against the reference table, checks
that the rate-scaled designs keep the passband and the corner, and reports
the deviations the constant scale leaves in the stopband.

Usage: gen_filter_table.py [--dump]
  --dump    print the prototype's coefficients, one section per line
            (b0 b1 b2 a1 a2, Q30 integers), to stdout.
Emits C on stdout unless --dump; the checks and the response table go to
stderr.
"""

import cmath
import math
import sys

# The engine's anchor rate: the rate the prototype is designed at.
ANCHOR_RATE = 49096

# The reference's -3 dB frequency: the corner the rate scaling keeps in
# place (the research page's response table: -3.00 dB at 17.02 kHz).
CORNER_HZ = 17020.0

# The prototype's sections, in Q30 (the research page's candidate fit).
# The rows are: b0, b1, b2, a1, a2.
PROTOTYPE = [
    # H1: the first-order section (b2 and a2 are zero).
    (535498195, 535498195, 0, -2745435, 0),
    # H2: the low-Q biquad.
    (610876461, 1221752922, 610876461, 762572659, 607191361),
    # H3: the high-Q biquad (the pole radius is about 0.938).
    (953291420, 1906582839, 953291420, 1794846297, 944577557),
]

# The section's order (1 or 2), in the prototype's order.
ORDERS = [1, 2, 2]

# The reference response the prototype models (the research page's table):
# frequency in Hz, gain in dB.
REFERENCE_RESPONSE = [
    (100.0, 0.00),
    (5000.0, -0.11),
    (14000.0, 0.68),
    (14650.0, 0.78),
    (16000.0, -0.24),
    (17000.0, -2.96),
    (17020.0, -3.00),
    (18000.0, -6.87),
    (20000.0, -13.94),
]

# The tolerance of the anchor rate's response check, in dB.
TOLERANCE_DB = 0.05

# The tolerance of the rate-scaled designs' passband check, in dB: the
# passband (up to 5 kHz) and the corner must hold at every rate.
PASSBAND_TOLERANCE_DB = 0.2


def to_q30(values):
    """Q30 integers to floats."""
    return [v / (1 << 30) for v in values]


def kappa(rate):
    """The prototype's frequency-axis scale at `rate`.

    The scale keeps the response's -3 dB point at the reference's corner:
    the analog corner the design needs is the corner warped to the host's
    rate, and the prototype's own corner is the one warped to the anchor
    rate.
    """
    def warp(fs):
        return (fs / math.pi) * math.tan(math.pi * CORNER_HZ / fs)

    return warp(ANCHOR_RATE) / warp(rate)


def design(proto, order, rate, scale):
    """The section's digital coefficients at `rate`.

    The prototype's coefficients are Q30 integers; the transform runs in
    floats here (the library's own transform runs in fixed point, and the
    harness checks its result against the prototype at the anchor rate).
    """
    b0, b1, b2, a1, a2 = to_q30(proto)
    r = rate / ANCHOR_RATE

    # The inverse bilinear transform at the anchor rate: the section's
    # analog numerator and denominator.
    if order == 2:
        n0, n1, n2 = b0 + b1 + b2, 2.0 * (b0 - b2), b0 - b1 + b2
        d0, d1, d2 = 1.0 + a1 + a2, 2.0 * (1.0 - a2), 1.0 - a1 + a2
    else:
        n0, n1, n2 = b0 + b1, b0 - b1, 0.0
        d0, d1, d2 = 1.0 + a1, 1.0 - a1, 0.0

    # The prototype's frequency axis, scaled.
    n1, n2 = n1 * scale, n2 * scale * scale
    d1, d2 = d1 * scale, d2 * scale * scale

    # The forward bilinear transform at the host's rate.
    if order == 2:
        a0 = d0 + d1 * r + d2 * r * r
        a1n = 2.0 * d0 - 2.0 * d2 * r * r
        a2n = d0 - d1 * r + d2 * r * r
        b0n = (n0 + n1 * r + n2 * r * r) / a0
        b1n = (2.0 * n0 - 2.0 * n2 * r * r) / a0
        b2n = (n0 - n1 * r + n2 * r * r) / a0
    else:
        a0 = d0 + d1 * r
        a1n = d0 - d1 * r
        a2n = 0.0
        b0n = (n0 + n1 * r) / a0
        b1n = (n0 - n1 * r) / a0
        b2n = 0.0

    return (b0n, b1n, b2n), (1.0, a1n / a0, a2n / a0)


def response(sections, order_list, freq, rate):
    """The cascade's magnitude response in dB at `freq`."""
    z = cmath.exp(complex(0.0, 2.0 * math.pi * freq / rate))
    total = complex(1.0, 0.0)
    for (num_coeffs, den_coeffs), order in zip(sections, order_list):
        b0, b1, b2 = num_coeffs
        a0, a1, a2 = den_coeffs
        num = b0 + b1 / z + (b2 / (z * z) if order == 2 else 0.0)
        den = a0 + a1 / z + (a2 / (z * z) if order == 2 else 0.0)
        total *= num / den
    return 20.0 * math.log10(abs(total))


def dc_gain(sections, order_list):
    """The cascade's DC gain in dB (z = 1)."""
    total = 0.0
    for (num_coeffs, den_coeffs), order in zip(sections, order_list):
        b0, b1, b2 = num_coeffs
        a0, a1, a2 = den_coeffs
        num = b0 + b1 + b2
        den = a0 + a1 + a2
        total += 20.0 * math.log10(num / den)
    return total


def build(rate):
    """The cascade's sections at `rate`."""
    sections = []
    for proto, order in zip(PROTOTYPE, ORDERS):
        sections.append(design(proto, order, rate, kappa(rate)))
    return sections


def main():
    args = sys.argv[1:]
    if "--dump" in args:
        for proto in PROTOTYPE:
            print(" ".join(str(v) for v in proto))
        return

    print("the analog tail's filter prototype (the research page's candidate"
          " fit, Q30)", file=sys.stderr)
    for i, (proto, order) in enumerate(zip(PROTOTYPE, ORDERS)):
        coeffs = to_q30(proto)
        print(f"  H{i + 1} (order {order}): b = [{coeffs[0]:.8f}, {coeffs[1]:.8f},"
              f" {coeffs[2]:.8f}]  a = [1, {coeffs[3]:.8f}, {coeffs[4]:.8f}]",
              file=sys.stderr)

    ok = True

    # The prototype's own response, at the anchor rate.
    print(f"the response at the anchor rate ({ANCHOR_RATE} Hz):", file=sys.stderr)
    sections = build(ANCHOR_RATE)
    for freq, expect in REFERENCE_RESPONSE:
        got = response(sections, ORDERS, freq, ANCHOR_RATE)
        delta = got - expect
        if abs(delta) > TOLERANCE_DB:
            ok = False
            print(f"  {freq:8.0f} Hz: {got:+7.3f} dB (reference {expect:+.2f} dB,"
                  f" delta {delta:+.3f} dB) OUT OF TOLERANCE", file=sys.stderr)
        else:
            print(f"  {freq:8.0f} Hz: {got:+7.3f} dB (reference {expect:+.2f} dB)",
                  file=sys.stderr)

    # The rate-scaled designs: the same analog filter, sampled at another
    # rate.  The passband and the corner must hold; the stopband's shape
    # is warped with the rate (the scale is a constant), and the tool
    # reports the deviation rather than hiding it.
    for rate in (44100, 48000, 96000, 192000):
        print(f"the rate-scaled design at {rate} Hz (kappa ="
              f" {kappa(rate):.4f}):", file=sys.stderr)
        sections = build(rate)
        for freq, expect in REFERENCE_RESPONSE:
            got = response(sections, ORDERS, freq, rate)
            delta = got - expect
            note = ""
            if freq <= 5000.0 or abs(freq - CORNER_HZ) < 1.0:
                if abs(delta) > PASSBAND_TOLERANCE_DB:
                    ok = False
                    note = " OUT OF TOLERANCE"
            print(f"  {freq:8.0f} Hz: {got:+7.3f} dB (reference {expect:+.2f} dB,"
                  f" delta {delta:+.3f} dB){note}", file=sys.stderr)

    # The prototype's structure: the zeros at z = -1 (the numerator's
    # symmetry) and the DC gain's unity.
    for i, (proto, order) in enumerate(zip(PROTOTYPE, ORDERS)):
        b0, b1, b2 = to_q30(proto[:3])
        if order == 2:
            expected = (b0, 2.0 * b0, b0)
        else:
            expected = (b0, b0, 0.0)
        for got, want in zip((b0, b1, b2), expected):
            if abs(got - want) > 2.0 / (1 << 30):
                ok = False
                print(f"  H{i + 1}: the numerator is not the bilinear zero pair",
                      file=sys.stderr)
    dc = dc_gain(build(ANCHOR_RATE), ORDERS)
    if abs(dc) > 0.01:
        ok = False
        print(f"  the cascade's DC gain is off by {dc:+.4f} dB", file=sys.stderr)
    print(f"the cascade's DC gain: {dc:+.4f} dB", file=sys.stderr)

    print(f"response checks: {'ok' if ok else 'FAILED'}", file=sys.stderr)
    if not ok:
        sys.exit(1)

    # Emit C on stdout.
    print("/* Generated by tools/gen_filter_table.py -- do not edit.")
    print(" * The analog tail's reconstruction filter: the prototype cascade")
    print(" * at the engine's anchor rate (49,096 Hz), Q30.")
    print(" *")
    print(" * The rows are b0, b1, b2, a1, a2.  H1 is the first-order")
    print(" * section, H2 the low-Q biquad, H3 the high-Q biquad (its pole")
    print(" * radius is about 0.938).  The zeros sit at z = -1, so H1's")
    print(" * numerator is [1, 1] and H2's and H3's are [1, 2, 1].")
    print(" *")
    print(" * This is the research page's candidate fit of the VDX7 reference")
    print(" * response (a magnitude fit, not a circuit derivation - research")
    print(" * OQ-22).  The library maps it through the inverse bilinear")
    print(" * transform to its analog prototype and derives the digital")
    print(" * coefficients for the host's rate at context creation.")
    print(" */")
    print("#include <stdint.h>")
    print("const int32_t slopsix_filter_prototype[3][5] = {")
    names = ["H1 first order", "H2 biquad", "H3 biquad"]
    for i, proto in enumerate(PROTOTYPE):
        row = ", ".join(str(v) for v in proto)
        print(f"    /* {names[i]} */ {{ {row} }},")
    print("};")


if __name__ == "__main__":
    main()
