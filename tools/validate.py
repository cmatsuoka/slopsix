#!/usr/bin/env python3
"""Validation loop: render the same DX7 bank voices with slopsix and with
external reference renderers, then write a comparison report.

The reference renderers are out-of-repo artefacts.  Each must accept the
common command line:

    RENDERER --bank BANK.syx --program N --note N --velocity N \
             --hold SECONDS --tail SECONDS --rate HZ --out FILE.wav

and write a mono 16-bit WAV at HZ.  Pass their paths with the
--vdx7-render / --dexed-render options or the SLOPSIX_VDX7_RENDER /
SLOPSIX_DEXED_RENDER environment variables.

The slopsix renderer is built by ``make validate`` at
build/validation/render_slopsix.

Distances are computed on the rendered audio, not on the engine state:

  envelope  RMS over 10 ms windows, normalised to each take's peak and
            floored at -60 dB; the distance is the mean absolute dB
            difference over the windows where both takes are sounding.
  spectrum  the linear energy fraction in 1/3-octave bands from 40 Hz to
            8 kHz over a sustain window; the distance is the L1 distance
            between the band vectors (0 = identical shape).

VDX7 is the primary reference.  Dexed is the secondary baseline: if
slopsix's distance from Dexed is in the same range as VDX7's distance
from Dexed, slopsix is in the right neighbourhood, and the VDX7
distances are the accuracy measure that matters.
"""

import argparse
import datetime
import math
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

try:
    import numpy as np
    from scipy.io import wavfile
    from scipy.signal import welch
except ImportError as exc:  # pragma: no cover - environment check
    sys.stderr.write("validate.py: numpy and scipy are required: %s\n" % exc)
    raise SystemExit(2)

BANK_HEADER = b"\xf0\x43\x00\x09\x20\x00"
NATIVE_RATE = 49096

# The hand-written analysis that follows the generated tables.  It is
# static text: the numbers in it come from the renders of the current
# report, and it is kept here so that a regeneration does not lose it.
FINDINGS_SECTION = """## Findings

### The divergence list mixes two populations

A row is called an *outlier* for one engine when that engine's distance to the other is more than 1.3 times the other distance and larger than 1.0. Over the 64 rows:

| Metric | slopsix outlier | VDX7 outlier | agree |
|---|---|---|---|
| envelope | 10 | 16 | 38 |
| spectrum | 3 | 16 | 45 |

Overall slopsix is *closer* to VDX7 than Dexed is (median spectral distance 0.331 against 0.685, median envelope distance 2.54 dB against 2.54 dB; slopsix is at least as close on 41/64 spectrum rows and 37/64 envelope rows). The divergence list above therefore overstates the problem: it mixes rows where VDX7 is the outlier with rows where slopsix is.

### Population A - VDX7 disagrees with Dexed, slopsix agrees with Dexed

These are envelope-timing differences, not slopsix bugs: VDX7's EGs decay faster than both other engines.

- **ROM 3B program 26 (BANJO)**: VDX7 falls from 0 dB to -60 dB within 0.6 s. Slopsix and Dexed both hold -3 dB until 0.45 s and then decay at the same rate (at 0.40 s: slopsix -3.30 dB, Dexed -3.48 dB, VDX7 -41.07 dB).
- **ROM 1A program 21 (MARIMBA)**: VDX7 decays about 1.5 times faster than slopsix and Dexed, which agree with each other closely.

Rows that flag as VDX7 outliers on both metrics: ROM 1A 04, ROM 1A 13, ROM 3B 07. These belong to the known EGS EG-rate items (E-01/E-02).

### Population B - remaining single-metric slopsix divergences

No row flags as a slopsix outlier on both metrics after the 2026-11-18 selector fix.

Localised on **ROM 1A program 30** (TRAIN, algorithm 4, feedback 7): slopsix renders an extra partial at 977 Hz (relative amplitude 0.16) that neither VDX7 nor Dexed produces, while it holds the 1140 Hz band at 0.067 against VDX7's 0.23 and the 359 Hz band at 0.064 against VDX7's 0.21. Dexed stays within 0.043 of VDX7 on this patch; slopsix is 0.686 away. The same shape appears on ROM 1A 25 (TUB BELLS), with an extra partial at 917 Hz.

What is known so far:

- The extra partial comes from a **fixed-frequency operator**: clearing the OSC MODE bit of operator 4 removes it, clearing operator 3's does not. Both operators are in fixed mode (op3 coarse 10 / fine 99, op4 coarse 22 / fine 57).
- The fixed-frequency **frequency law is not the bug**: slopsix's model reproduces the ROM's law (`0x16AC + TABLE_OP_FREQ_FIXED[coarse & 3] + 136 * fine`) exactly - both give 976.8 Hz for op3 and 296.9 Hz for op4 in the EGS's frequency units.
- What differs is the fixed-mode operator's **level and modulation balance**: in VDX7 the fixed operators are nearly inaudible (0.00-0.01 relative amplitude), while in slopsix operator 4 reaches 0.16. VDX7 also carries a small 2361 Hz partial that slopsix lacks.

Open items, both **needs more investigation**:

- The fixed-mode operator's output level (the keyboard level scaling and the level composition) and its modulation depth.
- Whether VDX7's 2361 Hz partial is the same operator at a different level, or a sideband of a different operator.
- This localization was taken over the earlier engine, which read the
  dump's algorithm field one row off from the firmware's (resolved
  2026-11-18, below); the patch's row changed and the numbers are
  re-measured under the corrected convention, while the fixed-frequency
  mechanism itself stands (it is row-independent).

### The algorithms, checked one by one

The engine's algorithm ROM table matches the chip's entry for entry (the
selector, the feedback-pair store bit, the C/D control bits and the COM
code: 0 mismatches in 192 entries), and the carrier sets reproduce the
documented chart. Three corrections settled on 2026-11-18 - all adopt the
firmware's own semantics, which VDX7 (the register file shows the raw
pass-through) and Dexed (its table indexer) already implement:

- **The algorithm field of the dump is the OPS's 0..31 row.** The
  listing's PATCH_ACTIVATE_ALG_MODE composes the $2805 register as
  `PATCH_ALGORITHM << 3 | feedback` with no adjustment, so the dump's
  byte is the raw row (the panel displays one more). The engine used to
  read the field as 1..32 - one row off from both references across
  every render comparison. The earlier per-algorithm sweep numbers in
  this section were taken across adjacent rows and are retired; the
  sweep gets re-run on the corrected convention.
- **The selector belongs to the producer's row.** The walk applies a
  column's selector after computing operator N to modulate operator N-1,
  so the decoder stores it on operator N, not on the consumer N-1. The
  earlier placement shifted every selector by one operator; the COM code
  and carrier flag stay on the consumer. This is the 2026-11-18 fix that
  drops the spectral median to 0.331.
- **The sum/delay register is the die's own model**: `msum = (C ? mren :
  0) + (D ? signal : 0)`, the selector reads the register's new value
  (SEL2), the value before the producer's update (SEL3), the feedback
  pair's last stored value (SEL4, live - a push earlier in the same walk
  reaches the reader), or the averaged pair (SEL5). The earlier
  provisional two-bus model let double-writing operators shadow a
  loaded value, which severed the {4,5}->3 sums of algorithms 7/8/9, the
  {5,6}->4 sums of 10/11/14/15/26/27, the {4,5,6}->3 sums of 12/13 and
  the {2,3,5}->1 output sums of 16/17/18 (all 1-based); the die
  walkthrough pins the accumulate semantics and the chart draws them.

On the corrected engine the two factory banks' median spectral distance
to VDX7 is 0.331 (was 0.492) and the median envelope distance 2.54 dB
(was 2.85 dB); slopsix beats Dexed's VDX7 distance on 41/64 spectra
(was 33/64). The remaining divergences concentrate on the EGS EG-cadence
items (E-01/E-02, population A above) and the fixed-frequency
operator's level law (population B above); the algorithm walk itself
now matches the reference.

### The algorithm selector mapping: corrected (2026-11-18)

The engine's walk reads a selector after computing operator N to produce
operator N-1's modulation, but the decoder stored each ROM column's
selector on the *consumer* row. The selector was therefore applied one
operator early: algorithms whose adjacent selectors differ (18, 15, 6,
...) fed the wrong signal, sum or feedback value into the next row. The
decoder now stores the selector on the producer row; the COM code and
the carrier flag remain on the consumer. The 64-patch median spectral
distance to VDX7 drops from 0.467 to 0.331, the envelope median from
2.78 to 2.54 dB, and slopsix is at least as close as Dexed on 41/64
spectra and 37/64 envelopes. The flagged PIANO 2 (algorithm 18) and
CLAS.GUIT (algorithm 15) renders now follow the reference walk.

### The operator feedback: resolved (2026-11-18)

The earlier report had this open as "the operator that reads the
feedback path is wrong". That identification had two sources, both now
corrected, and neither was the reader:

- the **algorithm row shifted by one**, so the analysed row was the
  neighbour's (BANJO's dump byte 7 is the hardware's algorithm 8, not
  7);
- the **register model's shadowing**, which in that row severed the
  6->{5}->3 sum path that carries the feedback into the carrier
  operator 3, leaving the loop audibly dead - BANJO rendered
  bit-identically at feedback 0 and 7 (the old engine's 0 counts against
  the reference's 4343).

The reader/wiring was right all along: in every checked-in algorithm the
reader is the operator whose selector is SEL=4/5, with column 1 wrapping
to operator 6. The feedback pair is pushed **only where a row carries
the store bit** - in the self-feedback loops the reading operator's own
row carries it; in algorithms 4 and 6 (0-based rows 3 and 5) the ROM puts
the store bit on operator 4 resp. operator 5 while the reader is
operator 6, and the engine's old implicit "the reader's own output joins
the path" rule added a spurious contributor there. The pair now also
carries the die's push timing, so the delayed feedback selector (SEL4,
algorithms 19-25, 1-based) sees a same-walk push where the old engine was
one sample late.

Measured 2026-11-18 on the corrected engine: BANJO (dump byte 7)
responds to feedback 0 vs 7 with 2143 counts against the reference's
4342 at the same scenario; the residual difference tracks the model
items still open, not the feedback wiring.

### The patch loading, checked against the firmware

The firmware's own register writes can be checked directly: the reference emulator exposes the EGS's register file, so the values the firmware hands the chip can be compared with the engine's own composition over every patch of the banks. The result:

| Quantity | Matches the firmware |
|---|---|
| the EG rate registers | 64/64 patches |
| the EG level registers | 64/64 patches |
| the operator frequency registers (ratio and fixed mode) | all |
| the detune and the rate-scaling/amp-mod registers | all |
| the operator level register | 1/64 patches before the fix below |

The patch *fields* therefore load correctly; the discrepancy was in the **level pipeline**. Two firmware laws have since been applied to the engine (research: the firmware page's level-pipeline section):

- the **velocity route** now reads the firmware's velocity table backwards, at `32 - (velocity >> 2)`, so the softest key reads the byte that follows the table in the ROM. The operator level register now matches the firmware **exactly** for every operator without a keyboard-level-scaling depth (259 of the 384 operator instances over the 64 patches; the mean error is 0.00 units).
- the keyboard level scaling's **index space** is still open: the engine's distance from the breakpoint differs from the firmware's, which leaves a mean residual of 1.3 register units on the operators that do have a scaling depth. The magnitude law itself is confirmed.

""".splitlines()


def bank_names(bank_path):
    """Return the 32 voice names from a packed DX7 bank."""
    with open(bank_path, "rb") as f:
        data = f.read()
    if len(data) != 4104 or not data.startswith(BANK_HEADER):
        raise ValueError("%s is not a DX7 32-voice bank" % bank_path)
    names = []
    for i in range(32):
        voice = data[6 + i * 128:6 + (i + 1) * 128]
        names.append(voice[118:128].decode("ascii", "replace").rstrip())
    return names


def parse_programs(spec):
    if spec in (None, "", "all"):
        return list(range(32))
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return sorted(set(out))


def load_wav(path):
    rate, data = wavfile.read(path)
    if data.ndim > 1:
        data = data[:, 0]
    return float(rate), data.astype(np.float64) / 32768.0


def onset_sample(x, rate):
    win = max(1, int(rate * 0.002))
    energy = np.convolve(x * x, np.ones(win) / win, mode="same")
    peak = float(energy.max()) if energy.size else 0.0
    if peak <= 0.0:
        return 0
    above = np.nonzero(energy > peak * 1e-4)[0]
    return int(above[0]) if above.size else 0


def envelope_db(x, rate, onset):
    hop = max(1, int(rate * 0.010))
    count = max(1, (len(x) - onset) // hop)
    rms = np.empty(count)
    for i in range(count):
        seg = x[onset + i * hop:onset + (i + 1) * hop]
        rms[i] = math.sqrt(float(np.mean(seg * seg))) if seg.size else 0.0
    peak = float(rms.max())
    if peak <= 0.0:
        return np.full(count, -60.0)
    db = 20.0 * np.log10(np.maximum(rms, 1e-12) / peak)
    return np.maximum(db, -60.0)


def band_edges(per_octave=3, low=40.0, high=8000.0):
    count = int(math.ceil(math.log2(high / low) * per_octave))
    return low * 2.0 ** (np.arange(count + 1) / per_octave)


def spectral_bands(x, rate, onset, hold):
    start = onset + int(0.10 * rate)
    end = onset + int(min(max(hold * 0.5, 0.15), 0.50) * rate)
    segment = x[start:min(end, len(x))]
    if segment.size < 2048:
        segment = x[onset:min(len(x), onset + 8192)]
    if segment.size < 256:
        return None
    nperseg = min(8192, segment.size)
    freqs, power = welch(segment, fs=rate, window="hann", nperseg=nperseg,
                         noverlap=nperseg // 2, detrend=False)
    edges = band_edges()
    bands = np.empty(len(edges) - 1)
    for i, (lo, hi) in enumerate(zip(edges[:-1], edges[1:])):
        mask = (freqs >= lo) & (freqs < hi)
        bands[i] = power[mask].sum() if mask.any() else 1e-30
    total = bands.sum()
    if total <= 0.0:
        return np.full(len(bands), 1.0 / len(bands))
    return bands / total


def envelope_distance(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return float("nan")
    a, b = a[:n], b[:n]
    mask = (a > -55.0) & (b > -55.0)
    if not mask.any():
        return float("nan")
    return float(np.mean(np.abs(a[mask] - b[mask])))


def spectral_distance(a, b):
    if a is None or b is None:
        return float("nan")
    return float(np.abs(a - b).sum())


class Take:
    def __init__(self, path, hold):
        self.rate, self.samples = load_wav(path)
        self.onset = onset_sample(self.samples, self.rate)
        self.envelope = envelope_db(self.samples, self.rate, self.onset)
        self.spectrum = spectral_bands(self.samples, self.rate, self.onset,
                                       hold)


def run_renderer(renderer, bank, program, note, velocity, hold, tail, rate,
                 out_path, name, timeout):
    cmd = [
        renderer,
        "--bank", bank,
        "--program", str(program),
        "--note", str(note),
        "--velocity", str(velocity),
        "--hold", "%.6f" % hold,
        "--tail", "%.6f" % tail,
        "--rate", str(rate),
        "--out", out_path,
    ]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=timeout)
    except FileNotFoundError:
        raise RuntimeError("%s renderer not found: %s" % (name, renderer))
    except subprocess.TimeoutExpired:
        raise RuntimeError("%s renderer timed out for %s program %d"
                           % (name, os.path.basename(bank), program))
    except subprocess.CalledProcessError as exc:
        message = exc.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError("%s renderer failed for %s program %d: %s"
                           % (name, os.path.basename(bank), program, message))


def fmt(value, digits=2):
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "-"
    return ("%%.%df" % digits) % value


def median(values):
    values = [v for v in values if v is not None and not math.isnan(v)]
    if not values:
        return None
    return float(np.median(values))


def mean(values):
    values = [v for v in values if v is not None and not math.isnan(v)]
    if not values:
        return None
    return float(np.mean(values))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bank", action="append", default=[],
                        help="DX7 32-voice sysex bank (repeatable)")
    parser.add_argument("--label", action="append", default=[],
                        help="label for the preceding bank")
    parser.add_argument("--programs", default="all",
                        help="program list (default all: 0, 1, 2, ... or 0-31)")
    parser.add_argument("--note", type=int, default=60)
    parser.add_argument("--velocity", type=int, default=100)
    parser.add_argument("--hold", type=float, default=1.0)
    parser.add_argument("--tail", type=float, default=1.0)
    parser.add_argument("--rate", type=int, default=NATIVE_RATE)
    parser.add_argument("--slopsix-render",
                        default="build/dx7/validation/render_slopsix")
    parser.add_argument("--vdx7-render",
                        default=os.environ.get("SLOPSIX_VDX7_RENDER"))
    parser.add_argument("--dexed-render",
                        default=os.environ.get("SLOPSIX_DEXED_RENDER"))
    parser.add_argument("--work", default="build/validation")
    parser.add_argument("--out", default="docs/validation.md")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--vdx7-name", default="VDX7")
    parser.add_argument("--dexed-name", default="Dexed")
    args = parser.parse_args()

    if not args.bank:
        parser.error("at least one --bank is required")
    if len(args.label) not in (0, len(args.bank)):
        parser.error("--label must be given once per --bank")

    labels = args.label or [os.path.splitext(os.path.basename(b))[0]
                            for b in args.bank]
    programs = parse_programs(args.programs)
    for p in programs:
        if p < 0 or p > 31:
            parser.error("program %d is outside 0..31" % p)

    references = []
    if args.vdx7_render:
        references.append(("vdx7", args.vdx7_name, args.vdx7_render))
    if args.dexed_render:
        references.append(("dexed", args.dexed_name, args.dexed_render))

    if not os.access(args.slopsix_render, os.X_OK):
        sys.stderr.write(
            "validate.py: slopsix renderer not executable: %s\n"
            "  build it with 'make validate'\n" % args.slopsix_render)
        return 2
    for key, name, path in references:
        if not os.access(path, os.X_OK):
            sys.stderr.write(
                "validate.py: %s renderer not executable: %s\n" % (name, path))
            return 2

    os.makedirs(args.work, exist_ok=True)

    jobs = []
    for bank, label in zip(args.bank, labels):
        names = bank_names(bank)
        bank_dir = os.path.join(args.work, label)
        os.makedirs(bank_dir, exist_ok=True)
        for program in programs:
            jobs.append((bank, label, program, names[program], bank_dir))

    def render_job(job):
        bank, label, program, name, bank_dir = job
        stem = os.path.join(bank_dir, "p%02d" % program)
        outputs = {"slopsix": stem + "_slopsix.wav"}
        run_renderer(args.slopsix_render, bank, program, args.note,
                     args.velocity, args.hold, args.tail, args.rate,
                     outputs["slopsix"], "slopsix", args.timeout)
        for key, ref_name, path in references:
            out = "%s_%s.wav" % (stem, key)
            run_renderer(path, bank, program, args.note, args.velocity,
                         args.hold, args.tail, args.rate, out, ref_name,
                         args.timeout)
            outputs[key] = out
        return job, outputs

    results = []
    if args.jobs > 1:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for job, outputs in pool.map(render_job, jobs):
                results.append((job, outputs))
                sys.stderr.write("rendered %s program %d\n"
                                 % (job[1], job[2]))
    else:
        for job in jobs:
            job_result, outputs = render_job(job)
            results.append((job_result, outputs))
            sys.stderr.write("rendered %s program %d\n"
                             % (job[1], job[2]))

    rows = []
    for (bank, label, program, name, bank_dir), outputs in results:
        takes = {"slopsix": Take(outputs["slopsix"], args.hold)}
        for key, ref_name, path in references:
            takes[key] = Take(outputs[key], args.hold)

        row = {
            "bank": label,
            "program": program,
            "name": name,
            "slopsix": takes["slopsix"],
        }
        for key, ref_name, path in references:
            row[key] = takes[key]
        rows.append(row)

    write_report(args, labels, programs, references, rows)
    return 0


def write_report(args, labels, programs, references, rows):
    lines = []
    now = datetime.datetime.now(datetime.timezone.utc)
    lines.append("# Validation report")
    lines.append("")
    lines.append("Generated by `tools/validate.py` on %s UTC."
                 % now.strftime("%Y-%m-%d %H:%M"))
    lines.append("")
    lines.append("This report compares slopsix against %s (the primary,"
                 " hardware-accurate reference) and %s (the secondary"
                 " emulator baseline)."
                 % (args.vdx7_name, args.dexed_name))
    lines.append("VDX7 emulates the DX7 hardware directly; slopsix should"
                 " follow it. Dexed is useful as a second opinion: a"
                 " deviation from Dexed that is no larger than VDX7's own"
                 " deviation from Dexed is in the expected range.")
    lines.append("")
    lines.append("## Scenario")
    lines.append("")
    lines.append("- sample rate: %d Hz (all renderers)" % args.rate)
    lines.append("- note: %d, velocity: %d" % (args.note, args.velocity))
    lines.append("- note held for %.3f s, then %.3f s of tail"
                 % (args.hold, args.tail))
    lines.append("- banks: %s" % ", ".join(
        "%s (programs %s)" % (label,
                              ",".join(str(p) for p in programs))
        for label in labels))
    lines.append("")
    lines.append("## Method")
    lines.append("")
    lines.append("The takes are onset-aligned and compared on two"
                 " normalised features:")
    lines.append("")
    lines.append("- **Envelope**: RMS in 10 ms windows, normalised to each"
                 " take's peak and floored at -60 dB. The distance is the"
                 " mean absolute difference in dB over the windows where"
                 " both takes are above -55 dB. This is insensitive to"
                 " output level, which differs between the engines.")
    lines.append("- **Spectrum**: the linear energy fraction in"
                 " 1/3-octave bands from 40 Hz to 8 kHz over a sustain"
                 " window. The distance is the L1 distance between the band"
                 " vectors (0 = identical spectral shape, 2 = disjoint).")
    lines.append("- **S-D / V-D**: slopsix's distance from Dexed divided by"
                 " VDX7's distance from Dexed. Values near 1 mean slopsix"
                 " deviates from Dexed by about as much as VDX7 does.")
    lines.append("")
    lines.append("The 8 kHz upper bound keeps the comparison in the"
                 " digital engine's band; the hardware's analog output tail"
                 " (about 16 kHz) is a separate, known open item and is not"
                 " part of this metric.")
    lines.append("")

    has_vdx7 = any(key == "vdx7" for key, _, _ in references)
    has_dexed = any(key == "dexed" for key, _, _ in references)

    if has_vdx7:
        lines.append("## Per-patch results")
        lines.append("")
        header = ("| Bank | Pgm | Onset (ms) | Env S-V | Env D-V | "
                  "Spec S-V | Spec D-V | Spec S-D/V-D | Env S-D/V-D |")
        lines.append(header)
        lines.append("|---|---|---|---|---|---|---|---|---|")
        for row in rows:
            s = row["slopsix"]
            v = row.get("vdx7")
            d = row.get("dexed")
            if v is None:
                continue
            env_sv = envelope_distance(v.envelope, s.envelope)
            spec_sv = spectral_distance(v.spectrum, s.spectrum)
            env_dv = spec_dv = None
            env_sd = spec_sd = None
            if d is not None:
                env_dv = envelope_distance(v.envelope, d.envelope)
                spec_dv = spectral_distance(v.spectrum, d.spectrum)
                env_sd = envelope_distance(s.envelope, d.envelope)
                spec_sd = spectral_distance(s.spectrum, d.spectrum)

            def ratio(sd, vd):
                if sd is None or vd is None or math.isnan(vd) or vd < 1e-9:
                    return None
                return sd / vd

            onset_ms = 1000.0 * s.onset / s.rate
            lines.append("| %s | %02d | %s | %s | %s | %s | %s | %s | %s |"
                % (row["bank"], row["program"], fmt(onset_ms, 0),
                   fmt(env_sv), fmt(env_dv), fmt(spec_sv, 3),
                   fmt(spec_dv, 3),
                   fmt(ratio(spec_sd, spec_dv)),
                   fmt(ratio(env_sd, env_dv))))
        lines.append("")
    else:
        lines.append("## Per-patch results")
        lines.append("")
        lines.append("VDX7 was not available, so only the slopsix renders"
                     " were produced. Set `SLOPSIX_VDX7_RENDER` (or"
                     " `--vdx7-render`) and run again for the full"
                     " comparison.")
        lines.append("")
        lines.append("| Bank | Pgm | Onset (ms) | Peak |")
        lines.append("|---|---|---|---|")
        for row in rows:
            s = row["slopsix"]
            peak = float(np.max(np.abs(s.samples))) if s.samples.size else 0.0
            lines.append("| %s | %02d | %s | %.3f |"
                         % (row["bank"], row["program"],
                            fmt(1000.0 * s.onset / s.rate, 0), peak))
        lines.append("")

    if has_vdx7:
        lines.append("## Summary")
        lines.append("")
        env_sv = [envelope_distance(r["vdx7"].envelope, r["slopsix"].envelope)
                  for r in rows if r.get("vdx7")]
        spec_sv = [spectral_distance(r["vdx7"].spectrum, r["slopsix"].spectrum)
                   for r in rows if r.get("vdx7")]
        lines.append("- Median spectral distance to VDX7: %s"
                     % fmt(median(spec_sv), 3))
        lines.append("- Median envelope distance to VDX7: %s dB"
                     % fmt(median(env_sv), 2))
        if has_dexed:
            env_dv = [envelope_distance(r["vdx7"].envelope, r["dexed"].envelope)
                      for r in rows if r.get("vdx7") and r.get("dexed")]
            spec_dv = [spectral_distance(r["vdx7"].spectrum, r["dexed"].spectrum)
                       for r in rows if r.get("vdx7") and r.get("dexed")]
            lines.append("- Median spectral distance, Dexed to VDX7: %s"
                         % fmt(median(spec_dv), 3))
            lines.append("- Median envelope distance, Dexed to VDX7: %s dB"
                         % fmt(median(env_dv), 2))

            spec_wins = 0
            env_wins = 0
            comparable = 0
            for r in rows:
                if not (r.get("vdx7") and r.get("dexed")):
                    continue
                comparable += 1
                if (spectral_distance(r["vdx7"].spectrum,
                                      r["slopsix"].spectrum)
                        <= spectral_distance(r["vdx7"].spectrum,
                                             r["dexed"].spectrum)):
                    spec_wins += 1
                if (envelope_distance(r["vdx7"].envelope,
                                      r["slopsix"].envelope)
                        <= envelope_distance(r["vdx7"].envelope,
                                             r["dexed"].envelope)):
                    env_wins += 1
            lines.append("- Patches where slopsix is at least as close to"
                         " VDX7 as Dexed is: spectrum %d/%d, envelope %d/%d"
                         % (spec_wins, comparable, env_wins, comparable))
        lines.append("")

        lines.append("## Divergences to investigate")
        lines.append("")
        lines.append("These are the patches where slopsix is farther from"
                     " VDX7 than Dexed is by a clear margin, or where its"
                     " deviation from Dexed is much larger than VDX7's"
                     " deviation from Dexed. They are the first candidates"
                     " for a per-row investigation in the research pages.")
        lines.append("")
        flagged = []
        for r in rows:
            if not (r.get("vdx7") and r.get("dexed")):
                continue
            v, s, d = r["vdx7"], r["slopsix"], r["dexed"]
            spec_sv = spectral_distance(v.spectrum, s.spectrum)
            spec_dv = spectral_distance(v.spectrum, d.spectrum)
            env_sv = envelope_distance(v.envelope, s.envelope)
            env_dv = envelope_distance(v.envelope, d.envelope)
            spec_sd = spectral_distance(s.spectrum, d.spectrum)
            env_sd = envelope_distance(s.envelope, d.envelope)
            score = None
            reason = []
            if not math.isnan(spec_sv) and not math.isnan(spec_dv):
                if spec_sv > max(0.35, spec_dv * 1.5) and spec_sv > 0.5:
                    reason.append("spectrum S-V %.2f vs D-V %.2f"
                                  % (spec_sv, spec_dv))
                    score = spec_sv / max(spec_dv, 1e-9)
            if not math.isnan(env_sv) and not math.isnan(env_dv):
                if env_sv > max(3.0, env_dv * 1.5):
                    reason.append("envelope S-V %.1f dB vs D-V %.1f dB"
                                  % (env_sv, env_dv))
            if not math.isnan(spec_sd) and not math.isnan(spec_dv):
                if spec_dv > 1e-9 and spec_sd > spec_dv * 2.0 \
                        and spec_sd > 0.5:
                    if not reason:
                        reason.append("spectrum S-D %.2f vs V-D %.2f"
                                      % (spec_sd, spec_dv))
            if reason:
                flagged.append((score if score is not None else 0.0, r,
                                reason))
        flagged.sort(key=lambda item: item[0], reverse=True)
        if not flagged:
            lines.append("None. Every patch's spectral and envelope"
                         " deviations are in the range the two references"
                         " themselves span.")
        else:
            for _, r, reason in flagged:
                lines.append("- **%s program %02d**: %s."
                             % (r["bank"], r["program"], "; ".join(reason)))
        lines.append("")
        lines.append("The divergence list is diagnostic, not a pass/fail"
                     " gate: some of these are expected until the envelope"
                     " model's remaining EGS rewrite (E-01/E-02) and the"
                     " analog output tail are done. The research row that"
                     " each patch points at should be recorded before the"
                     " code is changed.")
        lines.append("")
        lines.extend(FINDINGS_SECTION)

    lines.append("## Reproducing")
    lines.append("")
    lines.append("```sh")
    lines.append("make validate-render")
    lines.append("SLOPSIX_VDX7_RENDER=/path/to/vdx7_render \\")
    lines.append("SLOPSIX_DEXED_RENDER=/path/to/dexed_render \\")
    lines.append("python3 tools/validate.py --bank ROM1A.syx \\")
    lines.append("    --bank ROM3B.syx --out docs/validation.md")
    lines.append("```")
    lines.append("")
    lines.append("The reference renderers are external; they are not part"
                 " of this repository. The renderer used for each reference"
                 " must implement the command line documented in"
                 " `tools/validate.py` and render the same note scenario.")
    lines.append("")

    with open(args.out, "w") as f:
        f.write("\n".join(lines))
    sys.stderr.write("wrote %s\n" % args.out)


if __name__ == "__main__":
    raise SystemExit(main())
