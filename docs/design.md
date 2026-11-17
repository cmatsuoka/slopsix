# Design and research ledger

This document is the project's index of research IDs: every tracked
question, what it asks, and a one-line summary of where it stands. The
codes are stable handles used across the code and the design notes; each
row records the resulting design fact so the code and the design can be
read together.

## Status legend

- `resolved` — settled with evidence; implementation can rely on it.
- `known` — believed settled; a small confirmation may remain.
- `partial` — narrowed, or only part of the item is settled.
- `open` — not settled; may need a capture or an engine decision.
- `decided` — settled by an explicit design choice, not by evidence.
- Evidence grades: `hardware` (die/board/manual facts), `firmware` (the
  instrument's own firmware), `model` (third-party model, hypothesis
  grade), `manual` (owner documentation), `design` (our choice).

## Scope

| ID | Item | Resolution |
|---|---|---|
| SC-01 | DX7 target variant | **Decided:** DX7 family (MK1 plus later-variant sound-path features), no per-variant switch. |
| SC-02 | DX100 target | **Decided:** 4 ops, 8 algorithms, 8-voice poly, mono, own patch surface, same graph core. |
| SC-03 | Extension scope | **Decided:** opsix-style extras behind a macro; core leaves hooks for operator types, routing and an alternative output stage. |
| SC-04 | Sine table & curves as data | **Decided:** hardware tables are generated from `tools/`; log-sine, exponential, note and COM tables are closed-form. |
| SC-05 | DX7-family delta map | **Decided:** velocity response, per-voice pan, micro-tuning and aftertouch routing are in engine scope; storage/MIDI/panel are host-side. |
| SC-06 | Third-party model evidence | **Partial:** model-derived facts are hypothesis-grade and must stay tagged; no DX100 model content. |
| SC-07 | DX7 hardware facts | **Partial (hardware):** 49,096 Hz; 22/23-bit phase arithmetic; closed-form tables; 12-bit amplitude domain; DAC + compander; algorithm graph decoded. |
| SC-08 | DX7 firmware facts | **Partial (firmware):** register map, note→frequency closed form, 660-unit value scale, LFO speed/delay laws, EG-bias composition, detune/bend/pitch-EG tables. |
| SC-09 | Library topology | **Decided:** one library, one public header, one set of entry-point names; the personalities are macro-gated engines, not parallel API families. No separate DX100 library. Sources stay flat, with `_dx7` / `_dx100` filename suffixes. |
| SC-10 | Personality selection and dispatch | **Decided:** the engine is chosen by a parameter at context creation and never changes; the render entry point is resolved at creation and stored in the context, so the render call is one indirect call per buffer with no personality branch in the sample loop or the voice loop. An engine whose macro is off is rejected at creation. |

## Pitch and frequency generation

| ID | Item | Resolution |
|---|---|---|
| P-01 | Note → frequency path | **Resolved:** closed-form note word; the per-key ±1¢ alternation is exact; engine works in the 4096-units-per-octave domain. Absolute Hz base remains inside the sound chip. |
| P-02 | Ratio-mode operator frequency | **Partial:** coarse table is exact; the fine/detune composition is model-derived (hypothesis). |
| P-03 | Fixed-frequency mode | **Partial:** fixed-frequency step tables are known; the Hz formula is model-derived. |
| P-04 | Detune | **Partial:** register format pinned (3-bit magnitude + sign); the log-unit step size is chip-internal. |
| P-05 | Pitch bend quantization | **Resolved (API):** 14-bit MIDI bend, hardware range and step, curve and quirks reproduced; one table-stride detail still provisional. |
| P-06 | Portamento | **Decided:** no engine portamento in v1; host glides. Rate law located for a later optional parameter. |
| P-07 | Micro-tuning (MK2) | **Open / deferred:** kept in scope; implementation waits on a table format. |

## Oscillator and phase modulation

| ID | Item | Resolution |
|---|---|---|
| O-01 | Phase accumulator | **Partial:** 22-bit increments / 23-bit adder pinned; engine uses a 32-bit state; wrap emulation and note-on phase are open. |
| O-02 | Sine table contents | **Known (recipe):** exact closed-form recipe, generated and internally verified. |
| O-03 | Amplitude application | **Partial:** 12-bit log₂ Q8 domain and 14-bit log word pinned; constants still to reconcile. |
| O-04 | Exponent conversion | **Known (recipe):** exact closed-form recipe, generated and verified. |
| O-05 | Modulation input scaling | **Partial:** five modulation sources defined; deviation scaling still open. |
| O-06 | Self-feedback | **Partial:** 3-bit parameter and feedback slot resolved; level→shift mapping is model-derived. |
| O-07 | Output-level 0–99 mapping | **Partial:** firmware level table known; model constants remain hypothesis-grade. |

## Envelopes and scaling

| ID | Item | Resolution |
|---|---|---|
| E-01 | EG stage shape | **Partial:** 4 rates/levels; curve is chip-internal, model-derived only. |
| E-02 | Rate → time mapping | **Partial:** model law only; chip-internal. |
| E-03 | Rate scaling | **Partial:** applied inside the sound chip; curves chip-internal. |
| E-04 | EG update cadence | **Partial:** operator EG is chip-internal; pitch EG is a ~159 Hz CPU staircase; LFO is a ~318 Hz static-level update. |
| E-05 | Keyboard level scaling | **Partial:** the runtime scaling-curve builder is decoded and reimplemented (43 key-code groups, the two 36-entry ROM curves, the depth law); the curve's overrun and the keybed's own breakpoint range are still open. |
| E-06 | Sustain / release details | **Partial:** edge cases (level 0 vs rate 0, mid-attack release) still model-derived. |
| E-07 | Velocity response | **Resolved:** the keybed is velocity sensitive (Mk1 included); velocity scales operator level; the direction is pinned (the operator volume register is an attenuation, so a harder key is louder); the keybed's own calibration is open. |
| E-08 | The level curve | **Resolved:** the hardware's 100-entry table is an attenuation table and the exact inverse of the level law (the hardware's curve below parameter 20, `28 + parameter` above it); the engine implements the law. |

## LFO and modulation routing

| ID | Item | Resolution |
|---|---|---|
| L-01 | Speed 0–99 → increment | **Partial:** closed firmware law; tick↔Hz divisor still open. |
| L-02 | Waveforms | **Partial:** six-wave enum and CPU-side waveform generation; byte selection provisional. |
| L-03 | Delay 0–99 | **Partial:** law `(16+u)<<(k+2)` decoded; one transcription doubt to confirm. |
| L-04 | Pitch depth (PMS) | **Resolved:** eight-entry depth table known exactly. |
| L-05 | Amplitude depth (AMD) | **Partial:** depth path and per-op reduction curve located; curve to be pinned. |
| L-06 | Sync and cadence | **Partial:** sync phase reset confirmed; mode bits to finish. |

## Algorithms and routing

| ID | Item | Resolution |
|---|---|---|
| A-01 | 32 algorithms as data | **Resolved:** alignment rule, carriers, feedback ops and COM counts verified; implementation-ready. |
| A-02 | Modulator→carrier summing | **Partial:** sum/delay register semantics defined; load/hold/add sequencing open. |
| A-03 | Feedback slot | **Resolved:** 3-bit field on the per-algorithm self-loop. |
| A-04 | Voice bus and pan | **Decided:** mono carrier bus; per-voice pan in the family API; pan law open. |
| A-05 | Per-algorithm output compensation | **Partial:** six COM log values and carrier counts verified; a known improvement over the model baseline. |

## Output stage

| ID | Item | Resolution |
|---|---|---|
| OU-01 | DAC and compander | **Partial:** 14-bit → 12-bit mantissa + 3-bit shift per voice; shift-code rule open. |
| OU-02 | Saturation and overflow | **Open:** summed-voice overflow behaviour not pinned. |
| OU-03 | Effective sample rate | **Resolved:** 49,096 Hz. |
| OU-04 | Analog tail | **Decided:** out of scope for v1; 3-bit master volume is a public parameter. |

## Voice, patch and host boundaries

| ID | Item | Resolution |
|---|---|---|
| V-01 | Patch format | **Partial:** 156-byte DX7 dump offsets known; later-variant deltas open. |
| V-02 | Voice allocation | **Decided:** 16 voices, deterministic oldest-first stealing. |
| V-03 | Controllers | **Resolved:** per-source range scaling, route flags, clamped amp-mod sum and one's-complemented EG bias. |
| V-04 | Performance parameters | **Resolved:** master tune exposed in Q16 cents; hardware scale and default documented. |
| V-05 | Patch memory and storage | **Decided:** host-side. |
| V-06 | MIDI | **Decided:** host-side. |
| V-07 | LCD and panel | **Decided:** host-side. |
| V-08 | Patch I/O shape | **Decided:** one size-parameterised `slopsix_set_voice`/`slopsix_get_voice` pair; the engine validates the dump size against its own (156 bytes DX7, 67 bytes DX100) and reports a range error on a mismatch. The dump's format stays a host concern. |
| V-09 | Shared calls across personalities | **Decided:** the calls both engines can serve are the same calls; the per-engine difference lives behind them and is reported through the error code (`SLOPSIX_ERR_UNSUPPORTED` for a feature an engine lacks). Setters return `slopsix_error`. The engine's shape is queryable (`slopsix_context_engine`, `slopsix_voice_count`, `slopsix_operator_count`, `slopsix_algorithm_count`, `slopsix_voice_dump_size`, `slopsix_context_sample_rate`), so host code can be written once. |

## DX100 specifics

| ID | Item | Resolution |
|---|---|---|
| D-01 | Parameter surface | **Partial:** 4 ops, 8 algorithms, 8-voice poly, 64 fixed ratios. The chip is YM2151/OPM; its complete register map is confirmed. Board-level die confirmation still desirable. |
| D-02 | DX100 EG | **Known:** ranges and init pinned; chip law: effective rate = `min(63, 2·R + Rks)`, RR uses `2·register + 1`, D1L in 3 dB steps, rate→time tables available. D1L polarity resolved: the firmware complements D1L (`0x8BE9`: `(~D1L & 0xF) << 4 | RR`), so UI D1L 15 = chip D1L 0 = loud sustain. |
| D-03 | Keyscale | **Resolved.** Rate scaling is chip-internal: `Rks = KC' >> (KS XOR 3)` (max 3/7/15/31), added as `2R+Rks`; RR input is `2·register+1`. Level scaling is firmware: per-operator depth `D = (LS·660)>>8`; 29-entry exponential shape in ROM `0x8C89` = `{0,1,2,3,4,5,6,7,8,9,11,14,16,19,23,28,33,39,47,57,67,80,95,113,134,160,190,224,255}` indexed by `i = (clamp(T[note]−14,0,112))>>2`; amount `(shape[i]·D)>>8`. Builder `0x8C05` adds a per-operator base from OUTPUT LEVEL (`OL<20`: table `0x8C75` = `{127,122,118,114,110,107,104,102,100,98,96,94,92,90,88,86,85,84,82,81}`, else `99−OL`) plus an algorithm offset (ALG 4: +8 on ops 2–3; ALG 5/6: +0x0D on ops 1–3; ALG 7: +0x10), doubled into the 29-byte per-operator RAM table `0x103A`. |
| D-04 | LFO | **Known (chip + firmware):** four waves; speed `(v·660)>>8`→LFRQ; LFRQ→Hz table; delay `(16+u)<<(k+2)`; PMS = 0/±5/±10/±20/±50/±100/±400/±700 cents; AMS = 0/23.9/47.8/95.6 dB; PMD 2's-complement, AMD binary; single global LFO (monotimbral). |
| D-05 | Pitch EG | **Known:** absent from the DX100 parameter surface. |
| D-06 | Velocity | **Resolved:** keybed not velocity sensitive; MIDI velocity only; internal fixed velocity `0x30` (=48); per-op 0–7 scaling on operator level. |
| D-07 | Output | **Partial:** mono output confirmed; the chip emits 10-bit mantissa + 3-bit exponent to a YM3012 DAC at φM = 3.579545 MHz. Board-level mono summing and analog stage still open. |
| D-08 | Patch format | **Resolved:** serialised voice is 67 bytes (32 × 67 per set); operator/common bit map decoded; two bytes/flags still unnamed. |
| D-09 | Personality isolation | **Decided:** behind `SLOPSIX_DX100`, compile-time separation. The DX100 is a separate *engine*, not a DX7 variant: it shares the graph representation and the API shell with the DX7 and nothing else in the per-sample path. It is selected at context creation (SC-10) and never changes; its parameter surface enters through its own 67-byte patch dump and the shared calls' per-engine semantics, never through parallel `slopsix_dx100_*` entry points. |

## Extension hooks

| ID | Item | Resolution |
|---|---|---|
| X-01 | General operator graph | **Open (design):** graph with per-op inputs/gains and carrier roles as data; specifiable from the decoded structure. |
| X-02 | Operator type field | **Open (design):** sine-PM is the DX7 kind; extras behind the macro. |
| X-03 | Per-operator filter slot | **Open (design):** extension-only. |
| X-04 | Feedback generality | **Open (design):** feedback as edges; DX7 is one self-loop. |
| X-05 | Parameter namespace | **Decided:** DX7 semantics frozen; extensions add parameters, never redefine them. |
| X-06 | Stereo/pan core | **Decided:** DX7-family per-voice pan; wider stereo/FX are extras. The DX100's hardware output is physically mono, and the personality keeps that signal in both channels by default. Its pan is a **balance of that one signal across the two channels**, built into the personality rather than gated behind `SLOPSIX_EXTRAS`: centre pan (the default) is the faithful output, and any other position is a convenience for a stereo host, not hardware behaviour — the accurate DX100 is the case where the host does not move the pan. |
| X-07 | Output-stage extensibility | **Decided:** separable compile-time-selected output stages; DX7 path has no runtime branch. |
| X-08 | Idealised bend curve | **Decided:** DX7 curve fixed; an extension may offer a smooth curve. |

## Fixed-point engineering

| ID | Item | Resolution |
|---|---|---|
| F-01 | Phase format | **Decided:** 32-bit state, 22-bit increments, 32-bit-safe; 23-bit wrap emulation open. |
| F-02 | Log-domain arithmetic | **Partial:** 12-bit `2^(v/256)` domain; extended-precision exponential recipe known. |
| F-03 | Multiply style | **Decided:** 64-bit intermediates; mulhi helper only if the target demands. |
| F-04 | Rate tables per sample rate | **Open:** table residency (shared const vs per-context derivation) to settle. |
| F-05 | Tables in flash | **Decided:** generators exist; note and COM generators done. |
| F-06 | Context budget | **Open:** to be documented at implementation. |

## Validation and test vectors

| ID | Item | Resolution |
|---|---|---|
| T-01 | Canonical patch set | **Open:** factory/custom patch sets are out-of-tree inputs; the harness must accept them by path. |
| T-02 | Hardware evidence set | **Open:** captures needed for feedback depth, EG/LFO laws, DAC/compander, per-algorithm loudness and velocity direction. |
| T-03 | Harness stages | **Partial:** planned table, EG, pitch, compander, pan, algorithm and LFO stages. |
| T-04 | Model comparison | **Partial:** secondary baseline only; the first concrete A/B target is the LFO speed law. |

## Cross-cutting open questions

| ID | Question | Resolution |
|---|---|---|
| OQ-01 | Are pitch EG / LFO / bend evaluated per sample or at CPU cadence? | **Answered:** pitch EG ~159 Hz, LFO ~318 Hz, bend static; staircases, not smooth. |
| OQ-02 | Master-tune unit and neutral value? | **Decided:** Q16 cents; 0 = shipped tuning; quantum documented. |
| OQ-03 | Meaning of the bend step words and their range mapping? | **Answered:** range = semitones 0–12, step = 0–15; one table-stride detail provisional. |
| OQ-04 | How does a 32-bit phase state emulate the 23-bit wrap? | **Open:** engine decision plus a capture. |
| OQ-05 | Note-on phase initialisation? | **Open:** needs a capture. |
| OQ-06 | Compander shift-code rule and saturation/overflow? | **Open:** needs capture/hardware facts. |
| OQ-07 | Deviation scaling of the five modulation sources? | **Open:** sets the FM index scale. |
| OQ-08 | Feedback level→shift map? | **Open:** model formula is not hardware. |
| OQ-09 | EG rate→increment, rate-scaling and key-level curves? | **Partial:** the key-level builder is decoded; operator EG law is chip-internal. |
| OQ-10 | Contents of the remaining scale/velocity/`statics` tables? | **Partial:** most tables located; the `statics` table is model-only. |
| OQ-11 | Pitch-EG tables and LFO delay increments? | **Answered:** both pitch-EG tables known; delay law decoded (one provisional byte). |
| OQ-12 | Which LFO speed law ships, and the tick↔Hz divisor? | **Partial:** closed firmware law ships; Hz divisor open. |
| OQ-13 | Shared const tables or per-context derivation? | **Open:** memory-budget decision. |
| OQ-14 | Per-voice pan law? | **Open:** no hardware rule pinned. |
| OQ-15 | Test-vector location and harness input path? | **Open:** out-of-tree inputs, consumed by path. |
| OQ-16 | Which frequency domain does the engine use? | **Decided:** the firmware's 4096-units-per-octave word; chip field is `>>2`. |
| OQ-17 | Reproduce the chip's 14-bit frequency-field truncation? | **Decided:** yes, quantize at the boundary. |
| OQ-18 | Which way does velocity go? | **Open:** audible direction known; internal representation inverted/complemented detail unresolved. |
| OQ-19 | DX100 internals | **Partial:** patch format (67 bytes), LFO laws/shared policy, internal velocity, operator on/off, the eight algorithms and the chip EG/rate laws are closed; the exact DX100 algorithm-number ↔ `CON` mapping and the board output stage remain open. |

## DX100 owner's-manual questions

| ID | Question | Resolution |
|---|---|---|
| OQ-DX100-01 | Chip identity (OPM-class?) | **Answered (chip level):** YM2151/OPM, 8 channels × 4 operators with M1/M2/C1/C2 and the full register map; board-level die proof still desirable. |
| OQ-DX100-02 | Full 8-algorithm topology and feedback operator | **Answered:** the eight `CON` topologies are decoded (chain, two-stack, parallel-carrier forms); feedback is on M1. |
| OQ-DX100-03 | Operator ON/OFF independence | **Answered:** independent 1-bit field; OFF forces TL to `0xFF`, still enumerated in the algorithm. |
| OQ-DX100-04 | EG rate→time and level curve | **Answered:** effective rate = `min(63, 2·R + Rks)` over a 6-bit split; the 10–90 % and 96 dB time tables are published; D1L is 3 dB per step, with `D1L=15` the chip's fully-decayed code. The firmware complements D1L before writing (`0x8BE9`), so UI 15 = chip 0 (loud sustain). |
| OQ-DX100-05 | Rate-scaling interpolation | **Answered:** `Rks[KC'][KS]` table added as `2R + Rks`; RR's input rate is `2·register + 1`. |
| OQ-DX100-06 | Level-scaling curve and start note | **Answered (firmware):** shape `0x8C89` (29 entries, above), depth `(LS·660)>>8`, index `(clamp(T[note]−14,0,112))>>2`, output-level base + algorithm offset, builder `0x8C05`, RAM table `0x103A`. The first non-zero index is note 14 (`T=18`); there is no breakpoint, polarity or curve choice. |
| OQ-DX100-07 | Detune step law and direction | **Partial:** the chip's DT1/DT2 tables are known; the DX100 UI −3…+3 → register mapping is firmware, still to pin. |
| OQ-DX100-08 | LFO speed/delay laws and waveform details | **Answered:** LFRQ→Hz table, the four W waveforms, PMD/AMD format, PMS and AMS tables; the delay ramp is the firmware law `(16+u)<<(k+2)`. |
| OQ-DX100-09 | Shared or per-voice LFO | **Answered:** one global LFO loaded from the selected voice; the instrument is monotimbral. |
| OQ-DX100-10 | Master-tune per-step quantity | **Answered (firmware):** `0x8F20` stores the signed 16-bit word `D = 4·(v−64)` at `0x007B` (stored tune 0…127, neutral 64). `D` is added to the note word before the T-law; its low byte becomes chip `KF`. One UI step = 4/256 semitone = 1/64 semitone = **1.5625 cents** (one chip KF LSB); range −100.0…+98.4 cents. The manual's ±100 cents is this law rounded. |
| OQ-DX100-11 | Fixed internal key velocity | **Answered:** `0x30` (=48). |
| OQ-DX100-12 | Patch/sysex byte layout | **Answered:** 67-byte serialised voice with full bit map; 32 × 67 per set. |
| OQ-DX100-13 | Output stage (sample rate, DAC, compander, saturation) | **Partial:** chip output is 10-bit mantissa + 3-bit exponent, external YM3012 DAC, φM known; board analog/mono summing still open. |
| OQ-DX100-14 | Panel algorithm-number ↔ `CON` permutation | **Partial:** three anchors (ALG1 = `CON 1`, ALG5 = `CON 4`, ALG8 = `CON 7`); the remaining five need a legible photo of the DX100 panel chart or the service manual. |

## DX100 firmware questions

| ID | Question | Resolution |
|---|---|---|
| OQ-DX100-FW-01 | Decode the serialised voice map | **Answered:** pack/unpack pair decoded; serialised voice is 67 bytes (32 × 67 per set). |
| OQ-DX100-FW-02 | LFO depth-curve index scaling and register law | **Answered:** `(v·660)>>8` index; LFRQ complemented except for sample-and-hold; PMD recomputed per tick. |
| OQ-DX100-FW-03 | Identify `0x00–0x07`, `0x09`, `0x0F`, `0x14` writes | **Answered:** chip map identifies `0x00–0x07` as test, `0x0F` as noise, `0x14` as timer/IRQ control, `0x18` as LFRQ and `0x19` as the combined PMD/AMD register; `0x09` is unused. |
| OQ-DX100-FW-04 | TL composition | **Resolved:** `TL = saturating (0x103A + velocity + EG_bias) >> 1`. `0x103A` is the per-op, per-key KLS+output-level+algorithm curve (D-03); velocity = `(KVS_low·vel >> 8) + KVS_high` from `0x102A/0x102E` (built by `0x8CA6`); EG_bias = `0x1002+op`, derived from EG BIAS SENS (`0x8B03` table `0x8B17`) through the `0x92E5` curve. Operator OFF forces `0xFF`. |
| OQ-DX100-FW-05 | Identify the `0x93E5` and `0x8663` tables | **Partial:** `0x93E5` is the test-register value table (it does have a code reference); `0x8663` still only used by the panel scan. |
| OQ-DX100-FW-06 | Test-mode entry from the panel | **Open:** special-key decoder located; full sequence untraced. |
| OQ-DX100-FW-07 | Shared-LFO policy | **Answered:** global LFO loaded at patch time; monotimbral, no per-voice LFO. |
| OQ-DX100-FW-08 | Patch fields → OPM register writes | **Open:** the 64 ratios → `MUL`/`DT1` (`0x40`), detune −3…+3 → `DT1`/`DT2`, AR/D1R/D2R → `0x80`/`0xA0`/`0xC0`, rate scaling → `KS`, BANK A–D → `AMS-EN`. The writes to `0x40`, `0x80`, `0xA0` and `0xC0` are not yet traced; the sound-init chain (`0x89B8`, `0x8995`) is where to look. |
| OQ-DX100-FW-09 | UI operator → chip slot (M1/M2/C1/C2) | **Open:** the firmware walks the operators in the order `{3,1,2,0}` (`0x8F1C`), but the chip's four slots are M1/M2/C1/C2 and the mapping is unknown. The algorithm table and the TL path depend on it. |
| OQ-DX100-FW-10 | The LFO's AMD register path | **Open:** `0x1A` is written once at `0xC26F` and the normal path was never traced; only the PMD side (`0x19`, recomputed per tick from `0x0FFC`) is known. |

## Notes

- Model-derived constants are always named and evidence-tagged in
  `src/slopsix_internal.h`; where firmware evidence contradicts a model,
  the firmware wins.
- The DX100 personality is compile-time isolated behind `SLOPSIX_DX100`;
  with a feature macro off, its code is absent from the per-sample path and
  asking for that engine at context creation fails.
- The public API's shape is settled but **not yet in the code**: the header
  still carries the Phase-1 signatures (`slopsix_create_context(rate)`,
  `slopsix_set_voice(ctx, bytes)`, `void` setters). The engine parameter,
  the error-returning setters, the size-parameterised patch I/O and the
  capability queries land before the DX100 personality (plan Phase 5b;
  decisions D-7, D-17…D-21).
- Two owner's-manual figures disagree with the chip and the chip wins:
  PMS maximum is ±700 cents (not ±800) and the LFO minimum is about
  0.008 Hz (not 0.0008 Hz).
