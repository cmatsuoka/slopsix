#!/usr/bin/env python3
"""Generate the analog tails' section descriptors.

The two personalities do not share a design:

  DX7   the DAC's zero-order hold and the board's op-amp network.  A
        gentle high-shelf cut models the sample-and-hold aperture's
        sinc(f / fs) droop; a 4th-order cascade of two bilinear
        (prewarped) biquad low-passes with staggered 16.0/16.5 kHz
        corners and gentle Q models the active low-pass stages.

  DX7S  the experimental descriptor design: the aperture shelf first,
        then a 24 dB/oct Butterworth low-pass at 21 kHz built from
        Q 0.5412 and Q 1.3066.

A descriptor row is

    kind, corner_millihz, q_tenk, gain_millidb

with kind 0 = identity, 1 = Vicanek-matched low-pass, 2 = high-shelf,
3 = bilinear (prewarped) low-pass.  The engine designs the digital
coefficients from the descriptors at the host's rate.

Usage: gen_filter_table.py [--dump]
  --dump    print the two descriptor tables, one row per line.
Emits C on stdout unless --dump; the response report goes to stderr.
"""

import cmath
import math
import sys

# The DAC's native clock: the rate the aperture droop is defined at.
ANCHOR_RATE = 49096

KIND_IDENTITY = 0
KIND_LOWPASS = 1
KIND_HIGHSHELF = 2
KIND_LOWPASS_BILINEAR = 3

# The DX7's proposed tail: the aperture shelf, then the staggered bilinear
# low-pass pair.
DX7_SHELF_HZ = 16500.0
DX7_SHELF_GAIN_DB = -3.9
DX7_SHELF_Q = 0.5
DX7_LP1_HZ = 16000.0
DX7_LP1_Q = 0.54
DX7_LP2_HZ = 16500.0
DX7_LP2_Q = 0.85

# The DX7S's unchanged descriptor design.
DX7S_SHELF_HZ = 16500.0
DX7S_SHELF_GAIN_DB = -3.4
DX7S_SHELF_Q = 0.5
DX7S_LP_HZ = 21000.0
DX7S_LP_Q1 = 0.5412
DX7S_LP_Q2 = 1.3066

REPORT_RATES = (44100, 48000, 49096, 96000)
REPORT_FREQS = (1000.0, 12000.0, 16000.0, 20000.0)


def descriptor(f0, q, kind=KIND_LOWPASS, gain_db=0.0):
    return (kind, int(round(f0 * 1000.0)), int(round(q * 10000.0)),
            int(round(gain_db * 1000.0)))


def dx7_descriptors():
    return [descriptor(DX7_SHELF_HZ, DX7_SHELF_Q, KIND_HIGHSHELF,
                       DX7_SHELF_GAIN_DB),
            descriptor(DX7_LP1_HZ, DX7_LP1_Q, KIND_LOWPASS_BILINEAR),
            descriptor(DX7_LP2_HZ, DX7_LP2_Q, KIND_LOWPASS_BILINEAR)]


def dx7s_descriptors():
    return [descriptor(DX7S_SHELF_HZ, DX7S_SHELF_Q, KIND_HIGHSHELF,
                       DX7S_SHELF_GAIN_DB),
            descriptor(DX7S_LP_HZ, DX7S_LP_Q1),
            descriptor(DX7S_LP_HZ, DX7S_LP_Q2)]


def clamp_corner(f0, rate):
    return min(f0, 0.45 * rate)


def bilinear_lp(f0, q, rate):
    """The RBJ/bilinear low-pass (the C fixed-point design's reference)."""
    f0 = clamp_corner(f0, rate)
    w0 = 2.0 * math.pi * f0 / rate
    cos_w = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return ((1.0 - cos_w) / (2.0 * a0), (1.0 - cos_w) / a0,
            (1.0 - cos_w) / (2.0 * a0), -2.0 * cos_w / a0,
            (1.0 - alpha) / a0)


def vicanek_lp(f0, q, rate):
    """The Vicanek-matched low-pass (the DX7S design's reference)."""
    f0 = clamp_corner(f0, rate)
    w0 = 2.0 * math.pi * f0 / rate
    qq = 1.0 / (2.0 * q)
    a1 = -2.0 * math.exp(-qq * w0) * math.cos(
        w0 * math.sqrt(1.0 - qq * qq))
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
    return (b0, b1, 0.0, a1, a2)


def rbj_shelf(f0, q, gain_db, rate):
    """An RBJ high-shelf (the shared shelf design)."""
    f0 = clamp_corner(f0, rate)
    w0 = 2.0 * math.pi * f0 / rate
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    amp = 10.0 ** (gain_db / 40.0)
    root = math.sqrt(amp)
    alpha = sin_w / (2.0 * q)
    b0 = amp * ((amp + 1.0) + (amp - 1.0) * cos_w + 2.0 * root * alpha)
    b1 = -2.0 * amp * ((amp - 1.0) + (amp + 1.0) * cos_w)
    b2 = amp * ((amp + 1.0) + (amp - 1.0) * cos_w - 2.0 * root * alpha)
    a0 = (amp + 1.0) - (amp - 1.0) * cos_w + 2.0 * root * alpha
    a1 = 2.0 * ((amp - 1.0) - (amp + 1.0) * cos_w)
    a2 = (amp + 1.0) - (amp - 1.0) * cos_w - 2.0 * root * alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def sections(desc, rate):
    """The digital sections a descriptor table yields at `rate`."""
    out = []
    for kind, freq_mhz, q_tenk, gain_mdb in desc:
        f0 = freq_mhz / 1000.0
        q = q_tenk / 10000.0
        if kind == KIND_HIGHSHELF:
            out.append(rbj_shelf(f0, q, gain_mdb / 1000.0, rate))
        elif kind == KIND_LOWPASS_BILINEAR:
            out.append(bilinear_lp(f0, q, rate))
        elif kind == KIND_LOWPASS:
            out.append(vicanek_lp(f0, q, rate))
        else:
            out.append((1.0, 0.0, 0.0, 0.0, 0.0))
    return out


def response(desc, freq, rate):
    z = cmath.exp(complex(0.0, 2.0 * math.pi * freq / rate))
    total = complex(1.0, 0.0)
    for b0, b1, b2, a1, a2 in sections(desc, rate):
        total *= (b0 + b1 / z + b2 / (z * z)) / \
            (1.0 + a1 / z + a2 / (z * z))
    return 20.0 * math.log10(abs(total))


def report(label, desc):
    for rate in REPORT_RATES:
        values = " ".join(
            f"{freq / 1000.0:g}k {response(desc, freq, rate):+.1f} dB"
            for freq in REPORT_FREQS)
        print(f"  {label} at {rate} Hz: {values}", file=sys.stderr)


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

    report("DX7", dx7)
    report("DX7S", dx7s)

    print("/* Generated by tools/gen_filter_table.py -- do not edit.")
    print(" * The analog tails' reconstruction section descriptors.")
    print(" *")
    print(" * A row is")
    print(" *   kind, corner_millihz, q_tenk, gain_millidb")
    print(" * with kind 0 identity, 1 Vicanek-matched low-pass, 2 high-shelf,")
    print(" * 3 bilinear (prewarped) low-pass.  The engine designs the")
    print(" * digital coefficients from the descriptors at the host's rate.")
    print(" *")
    print(" * DX7: the zero-order-hold aperture's sinc droop as a gentle")
    print(f" * high-shelf at {DX7_SHELF_HZ / 1000.0:g} kHz"
          f" ({DX7_SHELF_GAIN_DB:+.1f} dB, Q {DX7_SHELF_Q:g}), then a")
    print(f" * 4th-order bilinear cascade at {DX7_LP1_HZ / 1000.0:g} kHz"
          f" (Q {DX7_LP1_Q:g}) and")
    print(f" * {DX7_LP2_HZ / 1000.0:g} kHz (Q {DX7_LP2_Q:g}).")
    print(" *")
    print(" * DX7S: the aperture shelf at 16.5 kHz (-3.4 dB, Q 0.5), then a")
    print(" * Vicanek-matched 4th-order Butterworth at 21 kHz (Q 0.5412,")
    print(" * 1.3066).  The DX7S table is compiled only into the DX7S")
    print(" * personality, so it does not reach the DX7 path.")
    print(" */")
    print("#include <stdint.h>")
    print("/* The DX7 tail: the aperture shelf, then the bilinear pair. */")
    emit_table("slopsix_filter_desc_dx7", dx7)
    print("#ifdef SLOPSIX_DX7S")
    print("/* The DX7S tail: the aperture shelf, then the Butterworth pair. */")
    emit_table("slopsix_dx7s_filter_desc", dx7s)
    print("#endif")


if __name__ == "__main__":
    main()
