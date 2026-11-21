#!/usr/bin/env python3
"""Generate the analog tails' section descriptors.

Each personality describes its reconstruction tail as up to three section
descriptors; the engine designs the digital coefficients from them at the
host's sample rate at context creation (a Vicanek-matched low-pass and an
RBJ high-shelf, see src/slopsix_filter.c).  The descriptors are the analog
intent, not an anchor-rate digital design:

  DX7   a single 12 dB/oct low-pass at 13.5 kHz, Q 0.68, fitted to stand in
        for the hardware's ~17 kHz analog low-pass plus the aperture shelf.

  DX7S  the aperture shelf first, then a 24 dB/oct Butterworth low-pass at
        21 kHz built from Q 0.5412 and Q 1.3066.  The shelf leads so its
        treble cut gives headroom before the resonating Q 1.3066 stage.

A descriptor row is

    kind, corner_millihz, q_tenk, gain_millidb

with kind 0 = identity, 1 = low-pass, 2 = high-shelf.  The aperture shelf
is a high-shelf cut fitted to the DAC's zero-order-hold response

    H_zoh(f) = | sin(pi f / fs) / (pi f / fs) |,   fs = 49,096 Hz.

It is not an inverse-sinc boost: both personalities are non-oversampled
~49.1 kHz DACs.

The checks below re-derive the digital design in floating point (the same
Vicanek and RBJ recipes the engine runs in fixed point) and report the
magnitude response at the anchor rate and at 44.1/48/96 kHz, so the table's
intent is visible.  The library clamps each corner to 0.45 fs.

Usage: gen_filter_table.py [--dump]
  --dump    print each personality's descriptors, one per line, to stdout.
Emits C on stdout unless --dump; the checks and the response table go to
stderr.
"""

import cmath
import math
import sys

# The hardware DAC's clock: the rate the aperture droop is defined at.
ANCHOR_RATE = 49096

# The personalities' analog low-pass sections, in Hz.
DX7_HZ = 13500.0
DX7_Q = 0.68
DX7S_HZ = 21000.0
DX7S_Q1 = 0.5412
DX7S_Q2 = 1.3066

# The zero-order-hold aperture shelf: a high-shelf cut fitted to the DAC's
# sinc droop.
SHELF_HZ = 16500.0
SHELF_GAIN_DB = -3.4
SHELF_Q = 0.5

# The hardware low-pass the DX7's single section stands in for, used only by
# the checks (its analog response is the target the fitted section tracks).
DX7_REF_LP_HZ = 17020.0
DX7_REF_LP_Q = 0.65
DX7_REF_PROBES = [8000.0, 12000.0, 16000.0, 20000.0]
DX7_REF_TOLERANCE_DB = 1.5

# The shelf's fit against the ZOH droop, in dB.
ZOH_PROBES = [10000.0, 15000.0, 19000.0, 20000.0, 22000.0]
ZOH_TOLERANCE_DB = 0.3

# The rates the checks report at.
REPORT_RATES = [44100, 48000, 49096, 96000]

# The descriptor kinds (mirrored in src/slopsix_internal.h).
KIND_IDENTITY = 0
KIND_LOWPASS = 1
KIND_HIGHSHELF = 2


def clamp_corner(f0, rate):
    """The corner, clamped to 0.45 of the rate (the engine's clamp)."""
    limit = 0.45 * rate
    return min(f0, limit)


def vicanek_lp(f0, q, rate):
    """Vicanek's matched low-pass at `rate` (floating point reference)."""
    f0 = clamp_corner(f0, rate)
    w0 = 2.0 * math.pi * f0 / rate
    qq = 1.0 / (2.0 * q)
    if qq <= 1.0:
        a1 = -2.0 * math.exp(-qq * w0) * math.cos(
            w0 * math.sqrt(1.0 - qq * qq))
    else:
        a1 = -2.0 * math.exp(-qq * w0) * math.cosh(
            w0 * math.sqrt(qq * qq - 1.0))
    a2 = math.exp(-2.0 * qq * w0)

    phi1 = math.sin(w0 / 2.0) ** 2
    phi0 = 1.0 - phi1
    phi2 = 4.0 * phi0 * phi1
    a0s = (1.0 + a1 + a2) ** 2
    a1s = (1.0 - a1 + a2) ** 2
    a2l = -4.0 * a2
    r1 = (a0s * phi0 + a1s * phi1 + a2l * phi2) * q * q
    b1s = (r1 - a0s * phi0) / phi1
    if b1s < 0.0:
        b1s = 0.0
    b0 = 0.5 * (math.sqrt(a0s) + math.sqrt(b1s))
    b1 = 0.5 * (math.sqrt(a0s) - math.sqrt(b1s))
    return (b0, b1, 0.0), (1.0, a1, a2)


def rbj_shelf(f0, gain_db, q, rate):
    """An RBJ high-shelf at `rate` (floating point reference)."""
    f0 = clamp_corner(f0, rate)
    amp = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / rate
    cosw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    root = math.sqrt(amp)
    b0 = amp * ((amp + 1.0) + (amp - 1.0) * cosw + 2.0 * root * alpha)
    b1 = -2.0 * amp * ((amp - 1.0) + (amp + 1.0) * cosw)
    b2 = amp * ((amp + 1.0) + (amp - 1.0) * cosw - 2.0 * root * alpha)
    a0 = (amp + 1.0) - (amp - 1.0) * cosw + 2.0 * root * alpha
    a1 = 2.0 * ((amp - 1.0) - (amp + 1.0) * cosw)
    a2 = (amp + 1.0) - (amp - 1.0) * cosw - 2.0 * root * alpha
    return ((b0 / a0, b1 / a0, b2 / a0), (1.0, a1 / a0, a2 / a0))


def identity():
    return (1.0, 0.0, 0.0), (1.0, 0.0, 0.0)


def descriptor_sections(desc, rate):
    """The digital sections a descriptor table yields at `rate`."""
    sections = []
    for kind, freq_mhz, q_tenk, gain_mdb in desc:
        if kind == KIND_LOWPASS:
            sections.append(vicanek_lp(freq_mhz / 1000.0,
                                       q_tenk / 10000.0, rate))
        elif kind == KIND_HIGHSHELF:
            sections.append(rbj_shelf(freq_mhz / 1000.0,
                                      gain_mdb / 1000.0,
                                      q_tenk / 10000.0, rate))
        else:
            sections.append(identity())
    return sections


def response(sections, freq, rate):
    """The cascade's magnitude response in dB at `freq`."""
    z = cmath.exp(complex(0.0, 2.0 * math.pi * freq / rate))
    total = complex(1.0, 0.0)
    for num_coeffs, den_coeffs in sections:
        b0, b1, b2 = num_coeffs
        a0, a1, a2 = den_coeffs
        num = b0 + b1 / z + b2 / (z * z)
        den = a0 + a1 / z + a2 / (z * z)
        total *= num / den
    return 20.0 * math.log10(abs(total))


def analog_lp(freq, f0, q):
    x = freq / f0
    return 20.0 * math.log10(
        1.0 / math.sqrt((1.0 - x * x) ** 2 + (x / q) ** 2))


def zoh_gain(freq):
    x = math.pi * freq / ANCHOR_RATE
    if x == 0.0:
        return 1.0
    return abs(math.sin(x) / x)


def descriptor(f0, q, kind=KIND_LOWPASS, gain_db=0.0):
    return (kind, int(round(f0 * 1000.0)), int(round(q * 10000.0)),
            int(round(gain_db * 1000.0)))


def dx7_descriptors():
    return [descriptor(DX7_HZ, DX7_Q),
            (KIND_IDENTITY, 0, 0, 0),
            (KIND_IDENTITY, 0, 0, 0)]


def dx7s_descriptors():
    return [descriptor(SHELF_HZ, SHELF_Q, KIND_HIGHSHELF, SHELF_GAIN_DB),
            descriptor(DX7S_HZ, DX7S_Q1),
            descriptor(DX7S_HZ, DX7S_Q2)]


def check_shelf():
    ok = True
    for freq in ZOH_PROBES:
        got = 20.0 * math.log10(zoh_gain(freq))
        shelf = response([rbj_shelf(SHELF_HZ, SHELF_GAIN_DB, SHELF_Q,
                                    ANCHOR_RATE)], freq, ANCHOR_RATE)
        note = ""
        if abs(shelf - got) > ZOH_TOLERANCE_DB:
            ok = False
            note = " OUT OF TOLERANCE"
        print(f"  shelf at {freq / 1000.0:g} kHz: {shelf:+.2f} dB,"
              f" ZOH {got:+.2f} dB{note}", file=sys.stderr)
    return ok


def check_dx7():
    ok = True
    sections = descriptor_sections(dx7_descriptors(), ANCHOR_RATE)
    shelf = rbj_shelf(SHELF_HZ, SHELF_GAIN_DB, SHELF_Q, ANCHOR_RATE)
    for freq in DX7_REF_PROBES:
        got = response(sections, freq, ANCHOR_RATE)
        want = analog_lp(freq, DX7_REF_LP_HZ, DX7_REF_LP_Q) \
            + response([shelf], freq, ANCHOR_RATE)
        note = ""
        if abs(got - want) > DX7_REF_TOLERANCE_DB:
            ok = False
            note = " OUT OF TOLERANCE"
        print(f"  DX7 at {freq / 1000.0:g} kHz: {got:+.2f} dB,"
              f" reference {want:+.2f} dB{note}", file=sys.stderr)
    return ok


def report(label, desc):
    print(f"  {label}:", file=sys.stderr)
    for rate in REPORT_RATES:
        sections = descriptor_sections(desc, rate)
        dc = response(sections, 0.0, rate)
        at16 = response(sections, 16000.0, rate)
        at20 = response(sections, 20000.0, rate)
        print(f"    {rate} Hz: DC {dc:+.3f} dB, 16 kHz {at16:+.2f} dB,"
              f" 20 kHz {at20:+.2f} dB", file=sys.stderr)


def emit_table(name, desc):
    print(f"const int32_t {name}[3][4] = {{")
    for kind, freq_mhz, q_tenk, gain_mdb in desc:
        print(f"    {{ {kind}, {freq_mhz}, {q_tenk}, {gain_mdb} }},")
    print("};")
    print()


def main():
    dx7 = dx7_descriptors()
    dx7s = dx7s_descriptors()

    if "--dump" in sys.argv[1:]:
        for row in dx7:
            print(" ".join(str(v) for v in row))
        print()
        for row in dx7s:
            print(" ".join(str(v) for v in row))
        return

    ok = True
    print("the experimental tails (section descriptors):", file=sys.stderr)
    print(f"  DX7: low-pass {DX7_HZ:.0f} Hz Q {DX7_Q:g}", file=sys.stderr)
    print(f"  DX7S: high-shelf {SHELF_HZ:.0f} Hz {SHELF_GAIN_DB:+.1f} dB"
          f" Q {SHELF_Q:g}, then Butterworth {DX7S_HZ:.0f} Hz"
          f" (Q {DX7S_Q1:g}, {DX7S_Q2:g})", file=sys.stderr)

    ok = check_shelf() and ok
    ok = check_dx7() and ok
    report("DX7", dx7)
    report("DX7S", dx7s)

    print(f"response checks: {'ok' if ok else 'FAILED'}", file=sys.stderr)
    if not ok:
        sys.exit(1)

    print("/* Generated by tools/gen_filter_table.py -- do not edit.")
    print(" * The analog tails' reconstruction section descriptors.")
    print(" *")
    print(" * EXPERIMENTAL: this deliberately departs from the VDX7-derived")
    print(" * 5th-order fit.  A row is")
    print(" *   kind, corner_millihz, q_tenk, gain_millidb")
    print(" * with kind 0 identity, 1 low-pass, 2 high-shelf.  The engine")
    print(" * designs the digital coefficients from the descriptors at the")
    print(" * host's rate (Vicanek-matched low-pass, RBJ high-shelf).")
    print(" *")
    print(f" * The DX7 tail is a single low-pass at {DX7_HZ / 1000.0:g} kHz,"
          f" Q {DX7_Q:g},")
    print(f" * fitted to stand in for the {DX7_REF_LP_HZ / 1000.0:g} kHz")
    print(" * hardware low-pass plus the aperture shelf.")
    print(f" * The DX7S tail is the aperture shelf at {SHELF_HZ / 1000.0:g} kHz")
    print(f" * ({SHELF_GAIN_DB:+.1f} dB, Q {SHELF_Q:g}) first, then a")
    print(f" * 4th-order Butterworth low-pass at {DX7S_HZ / 1000.0:g} kHz"
          f" (Q {DX7S_Q1:g},")
    print(f" * {DX7S_Q2:g}).")
    print(" * The DX7S table is compiled only into the DX7S personality, so")
    print(" * it does not reach the DX7 path.")
    print(" */")
    print("#include <stdint.h>")
    print("/* The DX7 tail: one low-pass (low-pass plus shelf). */")
    emit_table("slopsix_filter_desc_dx7", dx7)
    print("#ifdef SLOPSIX_DX7S")
    print("/* The DX7S tail: the aperture shelf, then the Butterworth pair. */")
    emit_table("slopsix_dx7s_filter_desc", dx7s)
    print("#endif")


if __name__ == "__main__":
    main()
