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
| SC-03 | Extension scope | **Decided:** `SLOPSIX_EXTRAS` carries conveniences beyond the original hardware (the pan control first), not opsix-style synthesis changes; opsix-style operator types, routing and an alternative output stage are deferred to a separate future macro. The core leaves the hooks for them. |
| SC-04 | Sine table & curves as data | **Decided:** hardware tables are generated from `tools/`; log-sine, exponential, note and COM tables are closed-form. |
| SC-05 | DX7-family delta map | **Decided:** velocity response, micro-tuning and aftertouch routing are in engine scope; panning is a convenience beyond the hardware and sits behind `SLOPSIX_EXTRAS`; storage/MIDI/panel are host-side. |
| SC-06 | Third-party model evidence | **Partial:** model-derived facts are hypothesis-grade and must stay tagged; no DX100 model content. |
| SC-07 | DX7 hardware facts | **Partial (hardware):** 49,096 Hz; 22/23-bit phase arithmetic; closed-form tables; 12-bit amplitude domain; DAC + compander; algorithm graph decoded. |
| SC-08 | DX7 firmware facts | **Partial (firmware):** register map, note→frequency closed form, 660-unit value scale, LFO speed/delay laws, EG-bias composition, detune/bend/pitch-EG tables; EG rate/level registers are pre-quantized to 0..63 (`(rate·164)>>8`, `TABLE_LOG[level]>>1`). |
| SC-09 | TX81Z / DX11 target | **Evaluation:** 4 ops, 8 algorithms, 8-voice poly, 8-part multitimbral performance mode, 8 sine-derived waveforms, PEG, stereo `panset` output and an HD6303-family CPU; a potential third personality behind `SLOPSIX_TX81Z`. The impact assessment and rows TX-01…TX-11 live on the TX81Z research page; the decision is open. |
| SC-10 | Personality selection and dispatch | **Decided:** the engine is chosen by a parameter at context creation and never changes; the render entry point is resolved at creation and stored in the context, so the render call is one indirect call per buffer with no personality branch in the sample loop or the voice loop. An engine whose macro is off is rejected at creation. |
| SC-11 | Library topology | **Decided:** one library, one public header, one set of entry-point names; the personalities are macro-gated engines, not parallel API families. No separate DX100 library. Sources live under `src/<personality>/` with the personality suffix dropped from the file names; genuinely shared sources stay at the top of `src/`. |

## Pitch and frequency generation

| ID | Item | Resolution |
|---|---|---|
| P-01 | Note → frequency path | **Resolved:** closed-form note word; the per-key ±1¢ alternation is exact; engine works in the 4096-units-per-octave domain. Absolute Hz base remains inside the sound chip. |
| P-02 | Ratio-mode operator frequency | **Partial (firmware + model):** coarse table is exact; the fine composition is model-derived. |
| P-03 | Fixed-frequency mode | **Partial (firmware + model):** fixed-mode composition is pinned (`opPitch>>2` + detune; no voice pitch, master tune, bend, pitch EG or LFO); the Hz formula remains model-derived. |
| P-04 | Detune | **Resolved (firmware + model):** 4-bit register (3-bit magnitude + sign), −7..+7; one step is 1/1024 octave ≈1.17 cents (4 units in the engine's 4096/octave word). |
| P-05 | Pitch bend quantization | **Resolved (firmware):** 14-bit MIDI bend, hardware range and step, curve and quirks reproduced; the 48-byte step table is 16×3-byte groups with a big-endian word, stride confirmed. |
| P-06 | Portamento | **Decided:** no engine portamento in v1; host glides. Rate law located for a later optional parameter. |
| P-07 | Micro-tuning (MK2) | **Open / deferred:** kept in scope; implementation waits on a table format. |

## Oscillator and phase modulation

| ID | Item | Resolution |
|---|---|---|
| O-01 | Phase accumulator | **Partial (hardware + design):** 22-bit increments / 23-bit adder pinned; engine uses a 32-bit state with an explicit 23-bit mask (D-6); note-on phase follows OSC KEY SYNC (OQ-05); the sync reset value is chip-internal. |
| O-02 | Sine table contents | **Known (recipe):** exact closed-form recipe, generated and internally verified. |
| O-03 | Amplitude application | **Partial (hardware + firmware + model):** 12-bit log₂ Q8 domain and 14-bit log word pinned; the EGS composes the target as attenuation `(level_reg<<6)+(op_level<<4)` (clamped `0xFF0`); the engine's gain form and 4256 offset are the translation. |
| O-04 | Exponent conversion | **Known (recipe):** exact closed-form recipe, generated and verified. |
| O-05 | Modulation input scaling | **Resolved (model):** the 14-bit modulator output is added directly to the 12-bit sine phase index (one period = 4096 units); full-scale deviation ≈±4 periods, no clamp. |
| O-06 | Self-feedback | **Partial (firmware + hardware + model):** 3-bit parameter and feedback slot resolved; effective division `>>(1+(7−level))`; a capture is still the closing evidence. |
| O-07 | Output-level 0–99 mapping | **Partial (firmware + model):** `TABLE_LOG` is the exact level curve; the firmware writes `TABLE_LOG[level]>>1` (0..63) as the EGS EG-level register and the EGS target uses it directly. The 4256 offset remains model-grade. |

## Envelopes and scaling

| ID | Item | Resolution |
|---|---|---|
| E-01 | EG stage shape | **Partial (firmware + model):** 4 rates/levels; the firmware pre-quantizes `rate_reg=(rate·164)>>8` and `level_reg=TABLE_LOG[level]>>1` (0..63); the EGS model uses an inverted 12-bit attenuation target, attack jump `0x94C`, slope `(level>>8)+2`, and delay compression. |
| E-02 | Rate → time mapping | **Partial (firmware + model):** the EGS qrate/clock gate is model-derived: `qrate=rate_reg+rate_scaling` clamped to 63, `nshift=11−(qrate>>2)` below 44, `pshift=(qrate>>2)−11` above, fractional masks `0xAA/0xEA/0xEE/0xFE`; still model-grade. |
| E-03 | Rate scaling | **Partial (firmware + model):** shipped as a register; the model law is `rs=round(clamp((voicePitch>>8)−16,0,27)·sens/7)` in qrate units. |
| E-04 | EG update cadence | **Partial (firmware + hardware):** operator EG is chip-internal (model: one operator envelope per sample on a 16-bit master clock); pitch EG is a ~188 Hz CPU staircase; LFO is a ~375.3 Hz static-level update. |
| E-05 | Keyboard level scaling | **Partial (firmware):** the runtime scaling-curve builder is decoded and reimplemented (43 key-code groups, the two 36-entry ROM curves, the depth law); the curve's overrun, the keybed's breakpoint range, and the exact mapping into the EGS amplitude domain remain open. |
| E-06 | Sustain / release details | **Partial (model):** the model pins the stage machine (key-on S0, key-off S3 from the current level, stages 2/3 short-circuit at target, delay only in stages 0/1); still model-grade. |
| E-07 | Velocity response | **Resolved:** the keybed is velocity sensitive (Mk1 included); velocity scales operator level; the direction is pinned (the operator volume register is an attenuation, so a harder key is louder); the keybed's own calibration is open. |
| E-08 | The level curve | **Resolved:** the hardware's 100-entry table is an attenuation table and the exact inverse of the level law (the hardware's curve below parameter 20, `28 + parameter` above it); the engine implements the law. |

## LFO and modulation routing

| ID | Item | Resolution |
|---|---|---|
| L-01 | Speed 0–99 → increment | **Resolved (firmware + model):** closed law; 16-bit accumulator advanced once per OCF tick; the tick is ≈375.3 Hz (9.4265 MHz/8, reload 3140), giving ≈0.063..49.7 Hz. |
| L-02 | Waveforms | **Resolved (firmware):** six waves and the byte selection are decoded from the 16-bit phase. |
| L-03 | Delay 0–99 | **Resolved (firmware):** `(16+u)<<(k+2)` with `u=(99−delay)&15`, `k=(99−delay)>>4`; the earlier transcription doubt was a misreading. |
| L-04 | Pitch depth (PMS) | **Resolved:** eight-entry depth table known exactly. |
| L-05 | Amplitude depth (AMD) | **Partial (firmware + model):** the depth path is at `$30F0`; the model applies an attenuation addition `a += amp_mod << ams` (ams 0..3, clamp `0xFFF`); the msfa depth table is superseded as the leading model. |
| L-06 | Sync and cadence | **Resolved (firmware):** 16-bit phase; note-on sync seeds `0x7FFF`; the delay accumulator and fade-in factor reset. |

## Algorithms and routing

| ID | Item | Resolution |
|---|---|---|
| A-01 | 32 algorithms as data | **Resolved (hardware + model):** alignment rule, carriers, feedback ops and COM counts verified; cross-checked entry-for-entry against a bit-synchronous OPS model (algorithm 18 op-1 C bit corrected). |
| A-02 | Modulator→carrier summing | **Partial (hardware + model):** the sum/delay register's C/D bits decode as clear/load/hold/add (`msum=(C?mren:0)+(D?signal:0)`, `mren=msum`); FREN is the feedback-path push; SEL 0..5 picks none/signal/new mren/previous mren/fren1/self-feedback average; the last op's mren is the voice output. A capture still closes it. |
| A-03 | Feedback slot | **Resolved:** 3-bit field on the per-algorithm self-loop. |
| A-04 | Voice bus and pan | **Decided:** mono carrier bus; panning is an extra beyond the hardware (`SLOPSIX_EXTRAS`, see X-06); pan law open. |
| A-05 | Per-algorithm output compensation | **Resolved (hardware + model):** six COM log values and carrier counts verified; the five-carrier value is 38, not 35. |

## Output stage

| ID | Item | Resolution |
|---|---|---|
| OU-01 | DAC and compander | **Partial (hardware + model):** 14-bit → 12-bit mantissa + 3-bit shift per voice; the model selects the shift by magnitude (≥`0x1000` drop 3, ≥`0x800` drop 2, ≥`0x400` drop 1, else none); exact SF-code numbering and saturation remain open. |
| OU-02 | Saturation and overflow | **Open:** summed-voice overflow behaviour not pinned; the model does not cover saturation. |
| OU-03 | Effective sample rate | **Resolved:** 49,096 Hz (≈49,096.354 from the 9.4265 MHz master clock / 192; E-clock / 24). |
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
| X-02 | Operator type field | **Open (design):** sine-PM is the DX7 kind; extra operator types are deferred to a future extension macro, not `SLOPSIX_EXTRAS`. |
| X-03 | Per-operator filter slot | **Open (design):** extension-only. |
| X-04 | Feedback generality | **Open (design):** feedback as edges; DX7 is one self-loop. |
| X-05 | Parameter namespace | **Decided:** DX7 semantics frozen; extensions add parameters, never redefine them. |
| X-06 | Stereo/pan core | **Decided:** every supported instrument's hardware output is mono, so panning is not hardware behaviour. It is a convenience for a stereo host behind `SLOPSIX_EXTRAS`: centre pan (the default) is the faithful output, and any other position balances the single signal across the two channels. Without the macro `slopsix_set_pan` is a no-op and the output stays at centre. Wider stereo/FX remain future extras. |
| X-07 | Output-stage extensibility | **Decided:** separable compile-time-selected output stages; DX7 path has no runtime branch. An alternative stage belongs to a future extension macro, not `SLOPSIX_EXTRAS`. |
| X-08 | Idealised bend curve | **Decided:** DX7 curve fixed; a future extension macro may offer a smooth curve. |

## Fixed-point engineering

| ID | Item | Resolution |
|---|---|---|
| F-01 | Phase format | **Decided:** 32-bit state, 22-bit increments, 32-bit-safe; the 23-bit wrap is emulated by an explicit mask at the increment add (D-6, implemented). |
| F-02 | Log-domain arithmetic | **Partial:** 12-bit `2^(v/256)` domain; extended-precision exponential recipe known. |
| F-03 | Multiply style | **Decided:** 64-bit intermediates; mulhi helper only if the target demands. |
| F-04 | Rate tables per sample rate | **Decided (D-4):** shared const tables in flash, no per-context copies; rate-dependent values derived as integer ratios at context creation. |
| F-05 | Tables in flash | **Decided:** generators exist; note and COM generators done. |
| F-06 | Context budget | **Open:** to be documented at implementation. |

## Validation and test vectors

| ID | Item | Resolution |
|---|---|---|
| T-01 | Canonical patch set | **Open:** factory/custom patch sets are out-of-tree inputs; the harness must accept them by path. |
| T-02 | Hardware evidence set | **Open:** captures needed for O-06, A-02, E-01/E-02, L-05, compander/SF numbering, per-algorithm loudness and velocity direction. |
| T-03 | Harness stages | **Partial:** table, operator, frequency, algorithm/graph, EG, level, COM and bend stages implemented; compander, pan, API and LFO stages planned. |
| T-04 | Model comparison | **Partial:** secondary baseline only; L-01's law is resolved, the next A/B targets are the EG curves and compander. |

## Cross-cutting open questions

| ID | Question | Resolution |
|---|---|---|
| OQ-01 | Are pitch EG / LFO / bend evaluated per sample or at CPU cadence? | **Answered:** pitch EG ~188 Hz, LFO ~375.3 Hz, bend static; staircases, not smooth. |
| OQ-02 | Master-tune unit and neutral value? | **Decided:** Q16 cents; 0 = shipped tuning; quantum documented. |
| OQ-03 | Meaning of the bend step words and their range mapping? | **Answered:** range = semitones 0–12, step = 0–15; the 16×3-byte step table and big-endian word are confirmed. |
| OQ-04 | How does a 32-bit phase state emulate the 23-bit wrap? | **Decided:** D-6 — 32-bit state masked to 23 bits at the increment add (implemented). |
| OQ-05 | Note-on phase initialisation? | **Answered:** the patch's OSC KEY SYNC bit selects OPS mode (`0x30` sync / `0x50` free-run); the reset value is chip-internal. |
| OQ-06 | Compander shift-code rule and saturation/overflow? | **Partial (model):** the magnitude rule (drop 3/2/1/0 above `0x1000`/`0x800`/`0x400`) is decoded; exact SF-code numbering and saturation/overflow still need a capture. |
| OQ-07 | Deviation scaling of the five modulation sources? | **Answered (model):** the 14-bit modulator is added directly to the 12-bit sine index; full-scale ≈±4 periods, no clamp. |
| OQ-08 | Feedback level→shift map? | **Partial (model):** effective `>>(1+(7−level))`; a capture is the closing evidence. |
| OQ-09 | EG rate→increment, rate-scaling and key-level curves? | **Partial (firmware + model):** the key-level builder is decoded; the EG registers are pre-quantized to 0..63 and the EGS qrate/clock-gate law is model-derived. |
| OQ-10 | Contents of the remaining scale/velocity/`statics` tables? | **Partial:** most tables located; the `statics` table is model-only. |
| OQ-11 | Pitch-EG tables and LFO delay increments? | **Answered:** both pitch-EG tables known; the delay law `(16+u)<<(k+2)` is confirmed, with no transcription error. |
| OQ-12 | Which LFO speed law ships, and the tick↔Hz divisor? | **Resolved:** the closed firmware law ships; the tick is ≈375.3 Hz from the 1.1783 MHz E-clock and the 3140-count reload. |
| OQ-13 | Shared const tables or per-context derivation? | **Decided:** D-4 — shared const tables; rate values derived at init. |
| OQ-14 | Per-voice pan law? | **Open:** no hardware rule pinned. |
| OQ-15 | Test-vector location and harness input path? | **Open:** out-of-tree inputs, consumed by path. |
| OQ-16 | Which frequency domain does the engine use? | **Decided:** the firmware's 4096-units-per-octave word; chip field is `>>2`. |
| OQ-17 | Reproduce the chip's 14-bit frequency-field truncation? | **Decided:** yes, quantize at the boundary. |
| OQ-18 | Which way does velocity go? | **Answered:** the operator volume register is an attenuation; the table is read from the hard end, so a harder key is louder. Keybed calibration remains open. |
| OQ-19 | DX100 internals | **Partial:** patch format (67 bytes), LFO laws/shared policy, internal velocity, operator on/off, the eight algorithms and the chip EG/rate laws are closed; the exact DX100 algorithm-number ↔ `CON` mapping and the board output stage remain open. |
| OQ-20 | Sum/delay register C/D operations and SEL source mapping? | **Partial (model):** C/D decode as clear/load/hold/add; SEL 0..5 maps none/signal/new mren/previous mren/fren1/self-feedback average; a register capture still closes it. |
| OQ-21 | Amplitude-modulation reduction law? | **Partial (model):** `a += amp_mod << ams` (ams 0..3, clamp `0xFFF`); the msfa depth table is superseded; a capture remains. |

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
