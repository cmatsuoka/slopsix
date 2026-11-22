#!/usr/bin/env python3
"""Generate the analog tails' reconstruction filter data.

The two personalities do not share a design:

  DX7   the original 5th-order cascade prototype: a first-order section
        followed by two biquads, all with their zeros at z = -1 (the
        bilinear low-pass zero).  The base prototype is the candidate fit
        distilled on /projects/slopsix/research/dx7-output - a magnitude
        fit of the VDX7 reference response over 0-20 kHz, not a circuit
        derivation (research OQ-22).  Its own -3 dB point is at 17.02 kHz,
        which is the DX7 tail's corner.  The prototype is the design at the
        engine's anchor rate (49,096 Hz); the engine keeps its analog form
        (the inverse bilinear transform) and re-derives the digital
        coefficients at the host's rate.

  DX7S  the experimental section-descriptor design: the aperture shelf
        first, then a 24 dB/oct Butterworth low-pass at 21 kHz built from
        Q 0.5412 and Q 1.3066.  The descriptors are the analog intent, not
        an anchor-rate digital design; the engine designs the coefficients
        at the host's rate (a Vicanek-matched low-pass and an RBJ
        high-shelf, see src/slopsix_filter.c).

A descriptor row is

    kind, corner_millihz, q_tenk, gain_millidb

with kind 0 = identity, 1 = low-pass, 2 = high-shelf.  The aperture shelf
is a high-shelf cut fitted to the DAC's zero-order-hold response

    H_zoh(f) = | sin(pi f / fs) / (pi f / fs) |,   fs = 49,096 Hz.

It is not an inverse-sinc boost: both personalities are non-oversampled
~49.1 kHz DACs.

The DX7 prototype is checked against the reference response and against
the rate-scaled designs.  Usage: gen_filter_table.py [--dump]
  --dump    print the DX7 prototype and the DX7S descriptors to stdout.
Emits C on stdout unless --dump; the checks and the response table go to
stderr.
"""

import cmath
import math
import sys

# The engine's anchor rate: the rate the DX7 prototype is designed at.
ANCHOR_RATE = 49096

# The DX7 tail's -3 dB corner, in Hz (the base fit's measured corner).
DX7_CORNER_HZ = 17020.0

# The DX7S's experimental descriptors: the aperture shelf and the
# Butterworth pair.
DX7S_SHELF_HZ = 16500.0
DX7S_SHELF_GAIN_DB = -3.4
DX7S_SHELF_Q = 0.5
DX7S_LP_HZ = 21000.0
DX7S_LP_Q1 = 0.5412
DX7S_LP_Q2 = 1.3066

# The descriptor kinds (mirrored in src/slopsix_internal.h).
KIND_IDENTITY = 0
KIND_LOWPASS = 1
KIND_HIGHSHELF = 2

# The base prototype's sections, in Q30 (the research page's candidate fit).
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

# The reference response the base prototype models (the research page's
# table): frequency in Hz, gain in dB.
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

TOLERANCE_DB = 0.05
CORNER_TOLERANCE_DB = 0.1
PASSBAND_TOLERANCE_DB = 0.25
MINUS_3DB = -3.0103


def to_q30(values):
    """Q30 integers to floats."""
    return [v / (1 << 30) for v in values]


def warp(corner, rate):
    """The bilinear prewarp of a corner at a rate."""
    return (rate / math.pi) * math.tan(math.pi * corner / rate)


def kappa(rate, corner):
    """The prototype's frequency-axis scale at `rate` for `corner`."""
    return warp(corner, ANCHOR_RATE) / warp(corner, rate)


def design(proto, order, rate, scale):
    """The section's digital coefficients at `rate` (floating point)."""
    b0, b1, b2, a1, a2 = to_q30(proto)
    r = rate / ANCHOR_RATE

    if order == 2:
        n0, n1, n2 = b0 + b1 + b2, 2.0 * (b0 - b2), b0 - b1 + b2
        d0, d1, d2 = 1.0 + a1 + a2, 2.0 * (1.0 - a2), 1.0 - a1 + a2
    else:
        n0, n1, n2 = b0 + b1, b0 - b1, 0.0
        d0, d1, d2 = 1.0 + a1, 1.0 - a1, 0.0

    n1, n2 = n1 * scale, n2 * scale * scale
    d1, d2 = d1 * scale, d2 * scale * scale

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


def build(protos, scale):
    """The cascade's sections, one per prototype row."""
    return [design(proto, order, ANCHOR_RATE, scale)
            for proto, order in zip(protos, ORDERS)]


def solve_scale(target):
    """The common axis scale that puts the cascade's -3 dB at `target`."""
    lo, hi = 0.2, 5.0
    for _ in range(120):
        mid = 0.5 * (lo + hi)
        got = response(build(PROTOTYPE, mid), ORDERS, target, ANCHOR_RATE)
        if got < MINUS_3DB:
            hi = mid
        else:
            lo = mid
    return 0.5 * (lo + hi)


def rescale_rows(scale):
    """The rescaled prototype's rows, as Q30 integers."""
    rows = []
    for (num_coeffs, den_coeffs) in build(PROTOTYPE, scale):
        b0, b1, b2 = num_coeffs
        a0, a1, a2 = den_coeffs
        rows.append(tuple(int(round(v * (1 << 30)))
                          for v in (b0, b1, b2, a1, a2)))
    return rows


def runtime_sections(rows, rate, corner):
    """The sections the library derives from `rows` at `rate`."""
    scale = kappa(rate, corner)
    return [design(row, order, rate, scale)
            for row, order in zip(rows, ORDERS)]


def check_rows(rows, corner, label):
    """The rescaled prototype's own checks: structure, DC and corner."""
    ok = True

    for i, (row, order) in enumerate(zip(rows, ORDERS)):
        b0, b1, b2 = to_q30(row[:3])
        if order == 2:
            expected = (b0, 2.0 * b0, b0)
        else:
            expected = (b0, b0, 0.0)
        for got, want in zip((b0, b1, b2), expected):
            if abs(got - want) > 64.0 / (1 << 30):
                ok = False
                print(f"  {label} H{i + 1}: the numerator is not the"
                      f" bilinear zero pair", file=sys.stderr)

    sections = runtime_sections(rows, ANCHOR_RATE, corner)
    dc = dc_gain(sections, ORDERS)
    if abs(dc) > 0.01:
        ok = False
        print(f"  {label}: the DC gain is off by {dc:+.4f} dB",
              file=sys.stderr)
    print(f"  {label}: the DC gain is {dc:+.4f} dB", file=sys.stderr)

    corner_gain = response(sections, ORDERS, corner, ANCHOR_RATE)
    if abs(corner_gain - MINUS_3DB) > CORNER_TOLERANCE_DB:
        ok = False
        print(f"  {label}: the corner gain is {corner_gain:+.3f} dB"
              f" (expected {MINUS_3DB:+.2f} dB) OUT OF TOLERANCE",
              file=sys.stderr)
    else:
        print(f"  {label}: the -3 dB corner at {corner:.0f} Hz is"
              f" {corner_gain:+.3f} dB", file=sys.stderr)

    for rate in (44100, 48000, 96000, 192000):
        scaled = runtime_sections(rows, rate, corner)
        got = response(scaled, ORDERS, corner, rate)
        flat = response(scaled, ORDERS, 1000.0, rate)
        note = ""
        if abs(got - MINUS_3DB) > CORNER_TOLERANCE_DB:
            ok = False
            note = " OUT OF TOLERANCE"
        if abs(flat) > PASSBAND_TOLERANCE_DB:
            ok = False
            note = " OUT OF TOLERANCE"
        print(f"  {label} at {rate} Hz: corner {got:+.3f} dB, 1 kHz"
              f" {flat:+.3f} dB{note}", file=sys.stderr)

    return ok


def descriptor(f0, q, kind=KIND_LOWPASS, gain_db=0.0):
    return (kind, int(round(f0 * 1000.0)), int(round(q * 10000.0)),
            int(round(gain_db * 1000.0)))


def dx7s_descriptors():
    return [descriptor(DX7S_SHELF_HZ, DX7S_SHELF_Q, KIND_HIGHSHELF,
                       DX7S_SHELF_GAIN_DB),
            descriptor(DX7S_LP_HZ, DX7S_LP_Q1),
            descriptor(DX7S_LP_HZ, DX7S_LP_Q2)]


def emit_prototype(name, rows, comment):
    print(f"/* {comment} */")
    print(f"const int32_t {name}[3][5] = {{")
    for row in rows:
        print(f"    {{ {row[0]}, {row[1]}, {row[2]}, {row[3]},"
              f" {row[4]} }},")
    print("};")
    print()


def emit_descriptors(name, desc):
    print(f"const int32_t {name}[3][4] = {{")
    for kind, freq_mhz, q_tenk, gain_mdb in desc:
        print(f"    {{ {kind}, {freq_mhz}, {q_tenk}, {gain_mdb} }},")
    print("};")
    print()


def main():
    dx7_rows = rescale_rows(solve_scale(DX7_CORNER_HZ))
    dx7s = dx7s_descriptors()

    if "--dump" in sys.argv[1:]:
        for row in dx7_rows:
            print(" ".join(str(v) for v in row))
        print()
        for row in dx7s:
            print(" ".join(str(v) for v in row))
        return

    ok = True

    print("the base prototype (the research page's candidate fit, Q30):",
          file=sys.stderr)
    base_sections = build(PROTOTYPE, 1.0)
    for freq, expect in REFERENCE_RESPONSE:
        got = response(base_sections, ORDERS, freq, ANCHOR_RATE)
        delta = got - expect
        if abs(delta) > TOLERANCE_DB:
            ok = False
            print(f"  {freq:8.0f} Hz: {got:+7.3f} dB (reference"
                  f" {expect:+.2f} dB, delta {delta:+.3f} dB)"
                  f" OUT OF TOLERANCE", file=sys.stderr)
        else:
            print(f"  {freq:8.0f} Hz: {got:+7.3f} dB (reference"
                  f" {expect:+.2f} dB)", file=sys.stderr)

    print(f"the DX7 prototype (base rescaled to {DX7_CORNER_HZ:.0f} Hz):",
          file=sys.stderr)
    ok = check_rows(dx7_rows, DX7_CORNER_HZ, "DX7") and ok

    print(f"response checks: {'ok' if ok else 'FAILED'}", file=sys.stderr)
    if not ok:
        sys.exit(1)

    print("/* Generated by tools/gen_filter_table.py -- do not edit.")
    print(" * The analog tails' reconstruction filters.")
    print(" *")
    print(" * DX7: the 5th-order cascade prototype at the engine's anchor")
    print(" * rate (49,096 Hz), Q30.  The rows are b0, b1, b2, a1, a2: H1")
    print(" * is the first-order section, H2 the low-Q biquad, H3 the")
    print(" * high-Q biquad.  Their zeros sit at z = -1, so H1's numerator")
    print(" * is [1, 1] and H2's and H3's are [1, 2, 1].  The base is the")
    print(" * research page's candidate fit of the VDX7 reference response")
    print(" * (a magnitude fit, not a circuit derivation - research OQ-22),")
    print(" * rescaled so the cascade's corner lands on the DX7 tail's")
    print(f" * measured -3 dB point at {DX7_CORNER_HZ / 1000.0:g} kHz.  The"
          " library keeps")
    print(" * the analog prototype and derives the digital coefficients for")
    print(" * the host's rate at context creation.")
    print(" *")
    print(" * DX7S: the experimental section descriptors.  A row is")
    print(" *   kind, corner_millihz, q_tenk, gain_millidb")
    print(" * with kind 0 identity, 1 low-pass, 2 high-shelf.  The tail is")
    print(f" * the aperture shelf at {DX7S_SHELF_HZ / 1000.0:g} kHz"
          f" ({DX7S_SHELF_GAIN_DB:+.1f} dB,")
    print(f" * Q {DX7S_SHELF_Q:g}) first, then a 4th-order Butterworth"
          f" low-pass at")
    print(f" * {DX7S_LP_HZ / 1000.0:g} kHz (Q {DX7S_LP_Q1:g},"
          f" {DX7S_LP_Q2:g}).  The engine designs")
    print(" * the digital coefficients from the descriptors at the host's")
    print(" * rate (Vicanek-matched low-pass, RBJ high-shelf).")
    print(" *")
    print(" * The DX7S table is compiled only into the DX7S personality,")
    print(" * so it does not reach the DX7 path.")
    print(" */")
    print("#include <stdint.h>")
    emit_prototype("slopsix_filter_prototype", dx7_rows,
                   "The DX7 tail: the base fit rescaled to a corner of"
                   f" {DX7_CORNER_HZ / 1000.0:g} kHz.")
    print("#ifdef SLOPSIX_DX7S")
    print("/* The DX7S tail: the aperture shelf, then the Butterworth"
          " pair. */")
    emit_descriptors("slopsix_dx7s_filter_desc", dx7s)
    print("#endif")


if __name__ == "__main__":
    main()
