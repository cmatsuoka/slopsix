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

Each bank patch is rendered at every requested note (default C4), so the
report carries the note dependence the hardware has (the keyboard level
scaling and the rate scaling are note-dependent).  Distances are
computed on the rendered audio, not on the engine state:

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

# The A/B/C comparison takes written for the report's worst cases: the
# slopsix, Dexed and VDX7 renders of one take, back to back with a short
# silence between, so that the three can be auditioned in order.
ABC_ORDER = ("slopsix", "dexed", "vdx7")
ABC_GAP_SECONDS = 0.25

# The hand-written analysis that follows the generated tables.  It is
# static text: the numbers in it come from the renders of the current
# report, and it is kept here so that a regeneration does not lose it.
FINDINGS_SECTION = """## Open questions

The remaining open accuracy items in this set are:

- **E-02/E-03/OQ-09**: the exact note-dependent `x` mapping in the EGS
  rate-scaling law, and a hardware capture of the envelope clock.
- **E-05/OQ-25**: the keyboard-level-scaling index space. The engine's
  operator level register still differs from the firmware's on the
  operators with a scaling depth (mean 1.3 register units).
- **O-07**: the 4000 level offset is calibrated against VDX7's raw
  operator signal, not a hardware capture; the absolute level remains
  model-grade.
- **A-02/OQ-20/OQ-27**: the hardware load/read phase and key-on
  persistence of the sum/delay and feedback registers.
- **O-06/OQ-08**: a hardware capture of the feedback level-to-shift path.
- **OU-01/OQ-06**: the compander's exact SF-code numbering and
  saturation/overflow behaviour.
- **T-02**: the hardware evidence set; the comparisons here use VDX7 and
  Dexed as model references.
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
        names.append(voice[118:128].decode("ascii", "replace").strip())
    return names


def expand_ints(spec):
    """Expand one spec string into ints: a comma list of ints and a-b
    (or a-b-step) ranges."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" not in part:
            out.append(int(part))
            continue
        fields = part.split("-")
        if len(fields) == 2:
            out.extend(range(int(fields[0]), int(fields[1]) + 1))
        else:
            out.extend(range(int(fields[0]), int(fields[1]) + 1,
                             max(1, int(fields[2]))))
    return out


def parse_choice(specs, what, low, high, default):
    """Parse an '--'-repeatable integer option into a sorted list of ints
    in low..high."""
    if specs in (None, [], "") or specs == ["all"]:
        specs = [default]
    try:
        ints = []
        for spec in specs:
            ints.extend(expand_ints(spec))
        out = sorted(set(ints))
    except ValueError:
        raise SystemExit("validate.py: bad %s list" % what)
    if not out:
        raise SystemExit("validate.py: empty %s list" % what)
    for value in out:
        if value < low or value > high:
            raise SystemExit("validate.py: %s %d is outside %d..%d"
                             % (what, value, low, high))
    return out


def parse_programs(spec):
    if spec in (None, "", "all"):
        return list(range(32))
    out = expand_ints(spec)
    out = sorted(set(out))
    for program in out:
        if program < 0 or program > 31:
            raise SystemExit("validate.py: program %d is outside 0..31"
                             % program)
    return out


NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
              "A#", "B")


def note_name(note):
    """The note's display name: MIDI 60 is C4, MIDI 69 is A4."""
    return "%s%d" % (NOTE_NAMES[note % 12], note // 12 - 1)


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
    """One rendered take's analysis.  The samples are dropped after the
    features are computed: a full run holds hundreds of takes, and the
    audio (about a megabyte each) is not needed once the envelope and
    the band vector exist."""

    def __init__(self, path, hold):
        self.rate, samples = load_wav(path)
        self.onset = onset_sample(samples, self.rate)
        self.peak = float(np.max(np.abs(samples))) if samples.size else 0.0
        self.envelope = envelope_db(samples, self.rate, self.onset)
        self.spectrum = spectral_bands(samples, self.rate, self.onset,
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
    parser.add_argument("--note", action="append", default=[],
                        help="note list, repeatable or comma-separated "
                             "(default 60; e.g. 36-96-24 sweeps the keyboard's "
                             "octaves two at a time, C2 to C6)")
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
    parser.add_argument("--abc-dir", default=None,
                        help="also write A/B/C comparison takes (slopsix, "
                             "Dexed, VDX7, %.2f s apart) for the report's "
                             "worst cases into this directory"
                             % ABC_GAP_SECONDS)
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
    notes = parse_choice(args.note, "note", 0, 127, 60)

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
            for note in notes:
                jobs.append((bank, label, program, names[program], note,
                             bank_dir))

    def render_job(job):
        bank, label, program, name, note, bank_dir = job
        stem = os.path.join(bank_dir, "p%02d_n%02d" % (program, note))
        outputs = {"slopsix": stem + "_slopsix.wav"}
        run_renderer(args.slopsix_render, bank, program, note,
                     args.velocity, args.hold, args.tail, args.rate,
                     outputs["slopsix"], "slopsix", args.timeout)
        for key, ref_name, path in references:
            out = "%s_%s.wav" % (stem, key)
            run_renderer(path, bank, program, note, args.velocity,
                         args.hold, args.tail, args.rate, out, ref_name,
                         args.timeout)
            outputs[key] = out
        return job, outputs

    results = []
    if args.jobs > 1:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for job, outputs in pool.map(render_job, jobs):
                results.append((job, outputs))
                sys.stderr.write("rendered %s program %d note %d\n"
                                 % (job[1], job[2], job[4]))
    else:
        for job in jobs:
            job_result, outputs = render_job(job)
            results.append((job_result, outputs))
            sys.stderr.write("rendered %s program %d note %d\n"
                             % (job[1], job[2], job[4]))

    # Group the takes per patch: one row per bank and program, with a
    # take set per note.  The analysis (the features the distances are
    # computed on) happens here, so the audio leaves memory immediately.
    rows = []
    index = {}
    for (bank, label, program, name, note, bank_dir), outputs in results:
        key = (bank, program)
        if key not in index:
            index[key] = {"bank": label, "program": program, "name": name,
                          "takes": {}}
            rows.append(index[key])
        takes = {"slopsix": Take(outputs["slopsix"], args.hold)}
        for ref_key, ref_name, path in references:
            takes[ref_key] = Take(outputs[ref_key], args.hold)
        index[key]["takes"][note] = takes
        index[key].setdefault("paths", {})[note] = outputs

    if args.abc_dir:
        write_abc_takes(args, rows)

    write_report(args, labels, programs, notes, references, rows)
    return 0


def patch_label(row):
    """The patch's display label: its name, or its program index if blank."""
    name = " ".join((row.get("name") or "").split())
    return name if name else "p%02d" % row["program"]


def row_notes(row):
    """The notes a patch row carries takes for, in order."""
    return sorted(row["takes"])


def note_distances(takes):
    """One take set's distances: slopsix against VDX7, and the Dexed
    baselines on the same take.  A distance is None where the reference
    is missing; the metric functions return NaN where no comparison
    window exists (both takes silent), which the mean/median helpers
    filter out."""
    s = takes["slopsix"]
    v = takes.get("vdx7")
    d = takes.get("dexed")
    out = {"env_sv": None, "spec_sv": None, "env_dv": None,
           "spec_dv": None, "env_sd": None, "spec_sd": None}
    if v is None:
        return out
    out["env_sv"] = envelope_distance(v.envelope, s.envelope)
    out["spec_sv"] = spectral_distance(v.spectrum, s.spectrum)
    if d is None:
        return out
    out["env_dv"] = envelope_distance(v.envelope, d.envelope)
    out["spec_dv"] = spectral_distance(v.spectrum, d.spectrum)
    out["env_sd"] = envelope_distance(s.envelope, d.envelope)
    out["spec_sd"] = spectral_distance(s.spectrum, d.spectrum)
    return out


def distance_ratio(sd, vd):
    if sd is None or vd is None or math.isnan(vd) or vd < 1e-9:
        return None
    return sd / vd


def patch_notes(row):
    """The patches' (take set, label) pairs, one per rendered note."""
    return [(note, "%s %s @ %s" % (row["bank"], patch_label(row),
                                   note_name(note)))
            for note in row_notes(row)]


def append_discrepancies(lines, rows):
    """List the current largest differences and the slopsix-only outliers."""
    spectral = []
    envelope = []
    spec_only = []
    env_only = []

    for row in rows:
        for note, label in patch_notes(row):
            if not (row["takes"][note].get("vdx7")
                    and row["takes"][note].get("dexed")):
                continue
            dist = note_distances(row["takes"][note])
            if not math.isnan(dist["spec_sv"]) and \
                    not math.isnan(dist["spec_dv"]):
                spectral.append((dist["spec_sv"], dist["spec_dv"], label))
                if dist["spec_sv"] > 1.0 and \
                        dist["spec_sv"] > 1.3 * dist["spec_dv"]:
                    spec_only.append((dist["spec_sv"], dist["spec_dv"],
                                      label))
            if not math.isnan(dist["env_sv"]) and \
                    not math.isnan(dist["env_dv"]):
                envelope.append((dist["env_sv"], dist["env_dv"], label))
                if dist["env_sv"] > 1.0 and \
                        dist["env_sv"] > 1.3 * dist["env_dv"]:
                    env_only.append((dist["env_sv"], dist["env_dv"], label))

    lines.append("## Current discrepancies")
    lines.append("")
    lines.append("Largest spectral differences to VDX7 (absolute S-V, top 5):")
    lines.append("")
    lines.append("| Take | Spec S-V | Spec D-V |")
    lines.append("|---|---|---:|")
    for sv, dv, label in sorted(spectral, key=lambda x: -x[0])[:5]:
        lines.append("| %s | %s | %s |" % (label, fmt(sv, 3), fmt(dv, 3)))
    lines.append("")
    lines.append("Largest envelope differences to VDX7 (absolute S-V, top 5):")
    lines.append("")
    lines.append("| Take | Env S-V | Env D-V |")
    lines.append("|---|---|---:|")
    for sv, dv, label in sorted(envelope, key=lambda x: -x[0])[:5]:
        lines.append("| %s | %s | %s |" % (label, fmt(sv), fmt(dv)))
    lines.append("")
    lines.append("Slopsix-specific outliers (S-V > 1.3 * D-V and S-V > 1.0,"
                 " the worst ten each):")
    lines.append("")
    if env_only or spec_only:
        if env_only:
            lines.append("- **Envelope**: " + ", ".join(
                "%s (%s vs %s dB)"
                % (label, fmt(sv), fmt(dv))
                for sv, dv, label in sorted(env_only,
                                            key=lambda x: -x[0])[:10]))
        if spec_only:
            lines.append("- **Spectrum**: " + ", ".join(
                "%s (%s vs %s)"
                % (label, fmt(sv, 3), fmt(dv, 3))
                for sv, dv, label in sorted(spec_only,
                                            key=lambda x: -x[0])[:10]))
    else:
        lines.append("None.")
    lines.append("")
    lines.append("The top lists are by slopsix's absolute distance to VDX7;"
                 " the D-V column shows how far Dexed is from VDX7 on the"
                 " same take. A take (@ the octave) is slopsix-specific"
                 " only when it exceeds both thresholds above.")
    lines.append("")


def worst_takes(rows):
    """The takes the report singles out, as (row, note) pairs: the largest
    absolute distances to VDX7 in both metrics and the slopsix-specific
    outliers, deduplicated and in report order."""
    env_abs = []
    spec_abs = []
    env_only = []
    spec_only = []
    for row in rows:
        for note in row_notes(row):
            takes = row["takes"][note]
            if not (takes.get("vdx7") and takes.get("dexed")):
                continue
            dist = note_distances(takes)
            if not math.isnan(dist["env_sv"]):
                env_abs.append((dist["env_sv"], row, note))
                if (not math.isnan(dist["env_dv"]) and dist["env_sv"] > 1.0
                        and dist["env_sv"] > 1.3 * dist["env_dv"]):
                    env_only.append((dist["env_sv"], row, note))
            if not math.isnan(dist["spec_sv"]):
                spec_abs.append((dist["spec_sv"], row, note))
                if (not math.isnan(dist["spec_dv"]) and dist["spec_sv"] > 1.0
                        and dist["spec_sv"] > 1.3 * dist["spec_dv"]):
                    spec_only.append((dist["spec_sv"], row, note))

    picks = sorted(env_abs, key=lambda x: -x[0])[:5]
    picks += sorted(spec_abs, key=lambda x: -x[0])[:5]
    picks += sorted(env_only, key=lambda x: -x[0])
    picks += sorted(spec_only, key=lambda x: -x[0])

    out = []
    seen = set()
    for _, row, note in picks:
        key = (row["bank"], row["program"], note)
        if key in seen:
            continue
        seen.add(key)
        out.append((row, note))
    return out


def write_abc_takes(args, rows):
    """Write the A/B/C comparison takes for the report's worst cases: the
    three engines' renders of one take in ABC_ORDER, ABC_GAP_SECONDS of
    silence between them.  The files are integer copies of the rendered
    takes; only the separation is added."""
    picks = worst_takes(rows)
    if not picks:
        sys.stderr.write("abc: no take has both references; skipped\n")
        return
    os.makedirs(args.abc_dir, exist_ok=True)
    for row, note in picks:
        paths = row["paths"][note]
        if not all(key in paths for key in ABC_ORDER):
            continue
        parts = []
        rate = None
        gap = None
        nominal = None
        for key in ABC_ORDER:
            take_rate, data = wavfile.read(paths[key])
            if data.ndim > 1:
                data = data[:, 0]
            if rate is None:
                rate = take_rate
                nominal = int(round((args.hold + args.tail) * rate))
                gap = np.zeros(int(round(ABC_GAP_SECONDS * rate)),
                               dtype=np.int16)
            segment = np.zeros(nominal, dtype=np.int16)
            count = min(nominal, len(data))
            segment[:count] = data[:count]
            if parts:
                parts.append(gap)
            parts.append(segment)
        label = row["bank"].lower()
        name = " ".join((row["name"] or "").split()).lower()
        name = name if name else "p%02d" % row["program"]
        out = os.path.join(args.abc_dir, "%s p%02d %s n%02d abc.wav"
                           % (label, row["program"], name, note))
        wavfile.write(out, rate, np.concatenate(parts))
    sys.stderr.write("wrote %d comparison takes to %s\n"
                     % (len(picks), args.abc_dir))


def write_report(args, labels, programs, notes, references, rows):
    lines = []
    now = datetime.datetime.now(datetime.timezone.utc)
    has_vdx7 = any(key == "vdx7" for key, _, _ in references)
    has_dexed = any(key == "dexed" for key, _, _ in references)
    rendered_notes = [note for note in notes
                      if any(note in row["takes"] for row in rows)]

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
    lines.append("- velocity: %d" % args.velocity)
    lines.append("- notes: %s"
                 % ", ".join("%d (%s)" % (note, note_name(note))
                             for note in rendered_notes))
    lines.append("- note held for %.3f s, then %.3f s of tail"
                 % (args.hold, args.tail))
    lines.append("- banks: %s" % ", ".join(labels))
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
    lines.append("Every patch is rendered at each note in the scenario."
                 " The per-patch rows aggregate over the notes (the mean of"
                 " each per-note distance and ratio); the per-octave table"
                 " shows the median distance of one octave across the whole"
                 " set. The keyboard level scaling and the envelope rate"
                 " scaling are note-dependent, so an octave whose distance"
                 " stands out points at those stages; the outlier lists"
                 " name the octave. The note sweep covers the DX7's own"
                 " keyboard span (the 61 keys C2 to C7, MIDI 36 to 96) two"
                 " octaves at a step.")
    lines.append("")

    if has_vdx7:
        lines.append("## Per-octave results")
        lines.append("")
        lines.append("The median distances at one note, across every patch"
                     " of the set:")
        lines.append("")
        lines.append("| Note | Env S-V | Env D-V | Spec S-V | Spec D-V |")
        lines.append("|---|---|---:|---:|---:|")
        for note in rendered_notes:
            dist = [note_distances(row["takes"][note]) for row in rows
                    if note in row["takes"]]
            lines.append("| %s | %s | %s | %s | %s |"
                % (note_name(note),
                   fmt(median([d["env_sv"] for d in dist])),
                   fmt(median([d["env_dv"] for d in dist])),
                   fmt(median([d["spec_sv"] for d in dist]), 3),
                   fmt(median([d["spec_dv"] for d in dist]), 3)))
        lines.append("")

        lines.append("## Per-patch results")
        lines.append("")
        header = ("| Bank | Patch | Onset (ms) | Env S-V | Env D-V | "
                  "Spec S-V | Spec D-V | Spec S-D/V-D | Env S-D/V-D |")
        lines.append(header)
        lines.append("|---|---|---|---|---|---|---|---|---|")
        for row in rows:
            if "vdx7" not in row["takes"][row_notes(row)[0]]:
                continue
            dist = [note_distances(row["takes"][note])
                    for note in row_notes(row)]
            onset = mean([1000.0 * row["takes"][note]["slopsix"].onset
                          / row["takes"][note]["slopsix"].rate
                          for note in row_notes(row)])
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s |"
                % (row["bank"], patch_label(row), fmt(onset, 0),
                   fmt(mean([d["env_sv"] for d in dist])),
                   fmt(mean([d["env_dv"] for d in dist])),
                   fmt(mean([d["spec_sv"] for d in dist]), 3),
                   fmt(mean([d["spec_dv"] for d in dist]), 3),
                   fmt(mean([distance_ratio(d["spec_sd"], d["spec_dv"])
                             for d in dist])),
                   fmt(mean([distance_ratio(d["env_sd"], d["env_dv"])
                             for d in dist]))))
        lines.append("")
    else:
        lines.append("## Per-patch results")
        lines.append("")
        lines.append("VDX7 was not available, so only the slopsix renders"
                     " were produced. Set `SLOPSIX_VDX7_RENDER` (or"
                     " `--vdx7-render`) and run again for the full"
                     " comparison.")
        lines.append("")
        lines.append("| Bank | Patch | Onset (ms) | Peak |")
        lines.append("|---|---|---|---|")
        for row in rows:
            onset = mean([1000.0 * row["takes"][note]["slopsix"].onset
                          / row["takes"][note]["slopsix"].rate
                          for note in row_notes(row)])
            peak = mean([row["takes"][note]["slopsix"].peak
                         for note in row_notes(row)])
            lines.append("| %s | %s | %s | %s |"
                         % (row["bank"], patch_label(row), fmt(onset, 0),
                            fmt(peak, 3)))
        lines.append("")

    if has_vdx7:
        lines.append("## Summary")
        lines.append("")
        pairs = [note_distances(row["takes"][note])
                 for row in rows for note in row_notes(row)]
        env_sv = [d["env_sv"] for d in pairs]
        spec_sv = [d["spec_sv"] for d in pairs]
        lines.append("- Median spectral distance to VDX7: %s"
                     % fmt(median(spec_sv), 3))
        lines.append("- Median envelope distance to VDX7: %s dB"
                     % fmt(median(env_sv), 2))
        if has_dexed:
            env_dv = [d["env_dv"] for d in pairs]
            spec_dv = [d["spec_dv"] for d in pairs]
            lines.append("- Median spectral distance, Dexed to VDX7: %s"
                         % fmt(median(spec_dv), 3))
            lines.append("- Median envelope distance, Dexed to VDX7: %s dB"
                         % fmt(median(env_dv), 2))

            spec_wins = spec_take = 0
            env_wins = env_take = 0
            for d in pairs:
                if not math.isnan(d["spec_sv"]) and \
                        not math.isnan(d["spec_dv"]):
                    spec_take += 1
                    if d["spec_sv"] <= d["spec_dv"]:
                        spec_wins += 1
                if not math.isnan(d["env_sv"]) and \
                        not math.isnan(d["env_dv"]):
                    env_take += 1
                    if d["env_sv"] <= d["env_dv"]:
                        env_wins += 1
            lines.append("- Takes where slopsix is at least as close to"
                         " VDX7 as Dexed is: spectrum %d/%d, envelope %d/%d"
                         % (spec_wins, spec_take, env_wins, env_take))
        lines.append("")

        append_discrepancies(lines, rows)
        lines.extend(FINDINGS_SECTION)
        lines.append("")

    lines.append("## Reproducing")
    lines.append("")
    lines.append("```sh")
    lines.append("make validate-render")
    lines.append("SLOPSIX_VDX7_RENDER=/path/to/vdx7_render \\")
    lines.append("SLOPSIX_DEXED_RENDER=/path/to/dexed_render \\")
    lines.append("python3 tools/validate.py --bank ROM1A.syx \\")
    lines.append("    --bank ROM2A.syx --bank ROM2B.syx --bank ROM3B.syx \\")
    lines.append("    --note 36,60,84 --out docs/validation.md")
    lines.append("```")
    lines.append("")
    lines.append("The reference renderers are external; they are not part"
                 " of this repository. The renderer used for each reference"
                 " must implement the command line documented in"
                 " `tools/validate.py` and render the same note scenario.")
    lines.append("")
    lines.append("Pass `--abc-dir DIR` to also write the A/B/C comparison"
                 " takes for the report's worst cases: the slopsix, Dexed"
                 " and VDX7 renders of one take, %.2f s of silence between"
                 " them." % ABC_GAP_SECONDS)
    lines.append("")

    with open(args.out, "w") as f:
        f.write("\n".join(lines))
    sys.stderr.write("wrote %s\n" % args.out)


if __name__ == "__main__":
    raise SystemExit(main())
