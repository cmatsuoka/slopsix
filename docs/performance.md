# Performance benchmark

This report compares the sound-generation cost of slopsix, Dexed and VDX7.
Only the render loop is measured: bank loading, context creation, the VDX7
firmware boot, host mixing, WAV encoding and file I/O are outside the timer.

## Environment and method

- **Date:** 2026-11-19
- **CPU:** AMD Ryzen 9 7900X (12 cores / 24 threads), pinned to one core
- **OS / compiler:** Linux, GCC/G++ 15.2.0
- **Sample rate:** 49,096 Hz (the DX7 native rate)
- **Audio measured:** 10 s per run after a 1 s warm-up
- **Runs:** 5 sequential runs per cell; the table shows the median.  The
  CPU governor was `powersave`, so the absolute frequency moved between
  runs.
- **Bank / patches:** ROM 1A, programs BRASS 1, E.PIANO 1 and SYN-LEAD 1
- **Single voice:** MIDI note 60
- **16 voices:** MIDI notes
  48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74

Each engine was driven through its own render entry point with a timing
harness that discards the audio after the engine's int16 output stage:

- **slopsix:** the default DX7 build (`make all`, `-Os` for the control
  sources and `-O3` for the per-sample path), one context with 16-voice
  polyphony.
- **Dexed:** the msfa Mark I engine (Dexed's default), one `Dx7Note` per
  held note, compiled at `-O2`.
- **VDX7:** the stock HD6303 + EGS + OPS core, compiled at `-O2` with
  `-DINLINE_EGS`.  Its own release Makefile adds `-O3 -ffast-math`, so
  these figures are conservative for VDX7.  VDX7 runs at its native rate;
  host resampling is not part of the measurement.

All runs produced non-silent output.  In the throughput column, values
above 1.00x are faster than slopsix and values below 1.00x are slower.

## Single voice

BRASS 1, note 60:

| Engine | Realtime factor | ns / frame | Throughput vs slopsix |
|---|---:|---:|---:|
| slopsix | 566x | 36.0 | 1.00x |
| Dexed | 1,160x | 17.6 | 2.05x |
| VDX7 | 20.7x | 986.1 | 0.04x |

## 16 voices

| Patch | Engine | Realtime factor | ns / frame | Throughput vs slopsix |
|---|---|---:|---:|---:|
| BRASS 1 | slopsix | 50.8x | 401.4 | 1.00x |
| BRASS 1 | Dexed | 74.6x | 273.1 | 1.47x |
| BRASS 1 | VDX7 | 18.6x | 1,095.3 | 0.37x |
| E.PIANO 1 | slopsix | 52.2x | 390.4 | 1.00x |
| E.PIANO 1 | Dexed | 76.2x | 267.1 | 1.46x |
| E.PIANO 1 | VDX7 | 18.8x | 1,084.9 | 0.36x |
| SYN-LEAD 1 | slopsix | 51.3x | 397.1 | 1.00x |
| SYN-LEAD 1 | Dexed | 95.6x | 213.0 | 1.86x |
| SYN-LEAD 1 | VDX7 | 19.2x | 1,058.6 | 0.37x |

## Interpretation

- **Dexed is about 1.5x to 2.0x faster than slopsix** in these scenarios.
  On one note the gap is 2.0x; at 16 voices it ranges from 1.46x (E.PIANO)
  to 1.86x (SYN-LEAD).
- **Slopsix costs scale with active voices.**  One note costs 36.0 ns per
  frame and 16 notes cost 390 to 401 ns, so the per-voice cost falls from
  36.0 to about 24 ns as fixed per-buffer work is amortised.  Dexed's cost
  is almost exactly linear in voices (17.6 ns for one, 16.7 to 17.1 ns per
  voice at 16), so the different scaling is part of why the gap depends on
  polyphony.
- **VDX7 is not a per-voice engine.**  It emulates the HD6303 firmware and
  clocks the EGS/OPS chip's full 16-voice array on every sample regardless
  of how many notes are held.  Its cost is therefore roughly constant at
  17x to 21x realtime; the small mono/poly difference is emulated
  firmware-path and clock noise, not voice scaling.
- Every engine has ample headroom on a single modern core: even the
  slowest 16-voice result is more than 17x realtime, and slopsix is at
  least 50x realtime.

The practical reading for slopsix is that its 12-bit-exact integer path
costs roughly 1.5 to 2 times Dexed's float path, while remaining
comfortably realtime for a full 16-voice chord.  VDX7's much lower figure
reflects the price of emulating the entire instrument rather than a
voice-level synthesis model.

## The per-block render path (2026-11-18)

The DX7 render call was reorganized to the reference emulator's block
shape: one operator renders a block of samples before the next starts,
the envelopes fill one level buffer per operator, every routing decision
resolves once per operator and block, and the render call is cut into
sub-blocks that end exactly on the LFO/pitch-EG ticks. The evaluation is
bit-identical to the previous per-sample one: all 32 algorithms' block
renders were checked against the per-sample walk from randomized carry-in
states (output and engine state, per span and per chunk boundary), the
whole public render path against a per-sample reference loop over every
split pattern and every tick position inside a block, and all 256 ROM
programs (eight banks, plus the forced-AMS runs) render byte-identically
to the previous build.

Measurement method: the local-only `local/bench` harness, ROM 1A, one
pinned CPU (`taskset`), interleaved old/new binaries, seven runs per
cell, median reported. The CPU governor was `powersave`, so only the
interleaved comparison should be trusted.

| Patch | Voices | before | after | gain |
|---|---:|---:|---:|---:|
| BRASS 1 | 1 | 41.1 | 34.0 | 17.3% |
| BRASS 1 | 16 | 494.5 | 390.3 | 21.1% |
| E.PIANO 1 | 1 | 40.4 | 33.4 | 17.5% |
| E.PIANO 1 | 16 | 450.5 | 379.9 | 15.7% |
| SYN-LEAD 1 | 1 | 40.6 | 33.4 | 17.7% |
| SYN-LEAD 1 | 16 | 512.2 | 388.8 | 24.1% |

ns / frame. The gain is wider at sixteen voices, which is where the
per-sample dispatch cost the change removes concentrates. The comparison
tables above were measured with this render path. Dexed's figures from
those tables are 17.6 ns/frame for one voice and 213.0 to 273.1 at
sixteen, so the remaining gap of about 1.5x to 2.0x is what the one
candidate 8 (the oscillator core) addresses: slopsix's per-sample
quantized oscillator and 12-bit DAC paths are kept deliberately.

The archive grew from 47 KB to 65 KB: the block renderer's specialized
runs inline the operator core a few more times than the walk did, in
the same speed-first translation unit as the rest of the per-sample
path (`src/slopsix_algorithm.c`). The table generators, the control-rate
sources and the public API glue stay size-first.

**Build-flag follow-up.** The growth was traced to compiling that
translation unit at `-O3`. With the schedule and block renderer at `-O2`
and the rest of the per-sample path still at `-O3`, the archive is about
56 KB instead of 70 KB, and the render is within 2-4% of the all-`-O3`
build (10-24% faster than an all-`-O2` build). `-Os` for the algorithm
translation unit is slower than `-O2`, so `-O2` is the floor there.

With this change the optimization candidates' big structural item is
done; the remaining candidates are the oscillator core (number 8) and
profile-guided optimization (number 9).
