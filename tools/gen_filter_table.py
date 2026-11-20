#!/usr/bin/env python3
"""Generate the analog tails' reconstruction filter prototypes.

The DX7's output path ends in an analog stage: the 12-bit DAC's output is
held by two alternating sample-and-hold circuits, summed, and low-passed by
a multi-section analog filter before the volume control.  The engine models
the filter (the sample-and-hold reconstruction stays out - research OQ-23).

The filter is modelled as a 5th-order cascade: a first-order section
followed by two biquads, all of them with their zeros at z = -1 (the
bilinear low-pass zero).  The base prototype below is the candidate fit
distilled on /projects/slopsix/research/dx7-output - a magnitude fit of the
VDX7 reference response over 0-20 kHz, not a circuit derivation (research
OQ-22).  Its own -3 dB point is at 17.02 kHz.

The personalities do not share one corner.  The DX7 tail keeps the base
fit's measured 17.02 kHz corner and the DX7S tail's is 20 kHz.  The DX7S
value is a placeholder: the service manual's output sheet shows the
Mk1-class RC values, but no DX7S response has been captured.  Each
prototype is the base fit with its frequency axis scaled so that the
cascade's -3 dB point lands on the personality's corner at the anchor
rate; the scale is solved here by bisection on the cascade response.  The
engine's per-rate scaling then holds that corner at the other host rates.

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

The engine also scales the prototype's frequency axis by a constant

    kappa = (fs0 / fs) * tan (pi fc / fs0) / tan (pi fc / fs)

so that the response's -3 dB point stays at the personality's corner `fc`
whatever the host's rate.

The tool checks the base prototype's response against the reference table,
then checks each rescaled personality prototype: its DC gain and zero
structure, its -3 dB point at the personality's corner, and that the
rate-scaled designs keep the passband and the corner.

Usage: gen_filter_table.py [--dump]
  --dump    print the base prototype's coefficients, one section per line
            (b0 b1 b2 a1 a2, Q30 integers), to stdout.
Emits C on stdout unless --dump; the checks and the response table go to
stderr.
"""

import cmath
import math
import sys

# The engine's anchor rate: the rate the prototype is designed at.
ANCHOR_RATE = 49096

# The personalities' corners, in Hz.  The base research fit's own corner is
# 17.02 kHz, and the DX7 tail keeps that measured corner.  The DX7S tail is
# rescaled to 20 kHz as a placeholder: the service manual shows Mk1-class
# filter values, not a verified 20 kHz corner.
DX7_CORNER_HZ = 17020.0
DX7S_CORNER_HZ = 20000.0

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

# The tolerance of the base prototype's response check, in dB.
TOLERANCE_DB = 0.05

# The tolerance of the rescaled prototypes' corner and passband checks, in
# dB.  The low passband (1 kHz) and the -3 dB corner must hold at every
# rate; the constant kappa warps the shape with the rate, so the extreme
# 192 kHz rate droops a little more than the audio rates.
CORNER_TOLERANCE_DB = 0.1
PASSBAND_TOLERANCE_DB = 0.25

# The cascade's -3 dB target, in dB.
MINUS_3DB = -3.0103


def to_q30(values):
    """Q30 integers to floats."""
    return [v / (1 << 30) for v in values]


def warp(corner, rate):
    """The bilinear prewarp of a corner at a rate."""
    return (rate / math.pi) * math.tan(math.pi * corner / rate)


def kappa(rate, corner):
    """The prototype's frequency-axis scale at `rate` for `corner`.

    The scale keeps the cascade's -3 dB point at `corner`: the analog corner
    the design needs is the corner warped to the host's rate, and the
    prototype's own corner is the one warped to the anchor rate.
    """
    return warp(corner, ANCHOR_RATE) / warp(corner, rate)


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


def build(protos, rates_scale):
    """The cascade's sections, one per prototype row."""
    return [design(proto, order, ANCHOR_RATE, rates_scale)
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
    """The sections the library derives from `rows` at `rate`, for `corner`."""
    scale = kappa(rate, corner)
    return [design(row, order, rate, scale)
            for row, order in zip(rows, ORDERS)]


def check_rows(rows, corner, label):
    """The rescaled prototype's own checks: structure, DC and corner."""
    ok = True

    # The prototype's zero structure: the bilinear zeros at z = -1.
    for i, (row, order) in enumerate(zip(rows, ORDERS)):
        b0, b1, b2 = to_q30(row[:3])
        if order == 2:
            expected = (b0, 2.0 * b0, b0)
        else:
            expected = (b0, b0, 0.0)
        for got, want in zip((b0, b1, b2), expected):
            # A few Q30 LSB: the base fit itself stores b1 one LSB off
            # 2*b0, and the rescale amplifies that to single-digit LSB.  It
            # is far below the response's accuracy.
            if abs(got - want) > 64.0 / (1 << 30):
                ok = False
                print(f"  {label} H{i + 1}: the numerator is not the bilinear"
                      f" zero pair", file=sys.stderr)

    sections = runtime_sections(rows, ANCHOR_RATE, corner)
    dc = dc_gain(sections, ORDERS)
    if abs(dc) > 0.01:
        ok = False
        print(f"  {label}: the DC gain is off by {dc:+.4f} dB", file=sys.stderr)
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

    # The rate-scaled designs: the corner and the low passband must hold.
    # The probe sits at 1 kHz, well below the corner: the base fit has a
    # resonant peak just under the corner, so near-corner flatness is not a
    # property of the design.
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


def emit_table(name, rows, comment):
    print(f"/* {comment} */")
    print(f"const int32_t {name}[3][5] = {{")
    for row in rows:
        print(f"    {{ {row[0]}, {row[1]}, {row[2]}, {row[3]}, {row[4]} }},")
    print("};")
    print()


def main():
    args = sys.argv[1:]
    if "--dump" in args:
        for proto in PROTOTYPE:
            print(" ".join(str(v) for v in proto))
        return

    ok = True

    # The base prototype's own response, at the anchor rate: the research
    # fit this table descends from.
    print("the base prototype (the research page's candidate fit, Q30):",
          file=sys.stderr)
    for i, (proto, order) in enumerate(zip(PROTOTYPE, ORDERS)):
        coeffs = to_q30(proto)
        print(f"  H{i + 1} (order {order}): b = [{coeffs[0]:.8f},"
              f" {coeffs[1]:.8f}, {coeffs[2]:.8f}]  a = [1,"
              f" {coeffs[3]:.8f}, {coeffs[4]:.8f}]", file=sys.stderr)

    print(f"the base response at the anchor rate ({ANCHOR_RATE} Hz):",
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

    # The two personalities' prototypes: the base fit rescaled so its
    # cascade corner is the personality's.
    dx7_scale = solve_scale(DX7_CORNER_HZ)
    dx7_rows = rescale_rows(dx7_scale)
    print(f"the DX7 prototype (base rescaled to {DX7_CORNER_HZ:.0f} Hz,"
          f" scale {dx7_scale:.6f}):", file=sys.stderr)
    ok = check_rows(dx7_rows, DX7_CORNER_HZ, "DX7") and ok

    dx7s_scale = solve_scale(DX7S_CORNER_HZ)
    dx7s_rows = rescale_rows(dx7s_scale)
    print(f"the DX7S prototype (base rescaled to {DX7S_CORNER_HZ:.0f} Hz,"
          f" scale {dx7s_scale:.6f}):", file=sys.stderr)
    ok = check_rows(dx7s_rows, DX7S_CORNER_HZ, "DX7S") and ok

    print(f"response checks: {'ok' if ok else 'FAILED'}", file=sys.stderr)
    if not ok:
        sys.exit(1)

    # Emit C on stdout.
    print("/* Generated by tools/gen_filter_table.py -- do not edit.")
    print(" * The analog tails' reconstruction filter prototypes at the")
    print(" * engine's anchor rate (49,096 Hz), Q30.")
    print(" *")
    print(" * The rows are b0, b1, b2, a1, a2.  H1 is the first-order")
    print(" * section, H2 the low-Q biquad, H3 the high-Q biquad.  The zeros")
    print(" * sit at z = -1, so H1's numerator is [1, 1] and H2's and H3's")
    print(" * are [1, 2, 1].")
    print(" *")
    print(" * The base is the research page's candidate fit of the VDX7")
    print(" * reference response (a magnitude fit, not a circuit")
    print(" * derivation - research OQ-22), rescaled so each personality's")
    print(" * cascade corner lands on its own -3 dB frequency: the DX7 at")
    print(f" * {DX7_CORNER_HZ / 1000.0:g} kHz, the DX7S at"
          f" {DX7S_CORNER_HZ / 1000.0:g} kHz (a placeholder; the service")
    print(" * manual shows Mk1-class filter values, not a verified corner).")
    print(" * The DX7S table is compiled")
    print(" * only into the DX7S personality, so it does not reach the DX7")
    print(" * path.")
    print(" */")
    print("#include <stdint.h>")
    emit_table("slopsix_filter_prototype", dx7_rows,
               f"The DX7 tail: the base fit rescaled to a corner of"
               f" {DX7_CORNER_HZ / 1000.0:g} kHz.")
    print("#ifdef SLOPSIX_DX7S")
    emit_table("slopsix_dx7s_filter_prototype", dx7s_rows,
               f"The DX7S tail: the base fit rescaled to a corner of"
               f" {DX7S_CORNER_HZ / 1000.0:g} kHz (placeholder).")
    print("#endif")


if __name__ == "__main__":
    main()
