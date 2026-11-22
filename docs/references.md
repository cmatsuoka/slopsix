# References

## Hardware analyses and die decapsulation

- **Ken Shirriff, "Reverse-engineering the Yamaha DX7" (parts 1–5)** —
  <https://www.righto.com/2021/11/reverse-engineering-yamaha-dx7.html>,
  the exp-ROM, log-sine, algorithm and output-circuitry instalments.
  Gave the chip-level architecture (OPS/EGS split, 49,096 Hz), the 22/23-bit
  phase arithmetic, the 16-cycle operator pipeline and 6→1 walk, the 96-entry
  algorithm ROM including SEL/A-C-D/COM fields, the closed-form log-sine and
  exponential table recipes, the carrier-count output compensation, and the
  output buffer, BA9221 DAC, scaler, sample-and-holds and Sallen-Key filter.
- **The 32-algorithm chart** (service-manual numbering) —
  <https://static.righto.com/images/dx7-alg/algorithms.jpg>. Used to verify
  the decoded carrier and feedback-operator sets, 32/32.
- **ajxs, "Yamaha DX7 Technical Analysis"** —
  <https://ajxs.me/blog/Yamaha_DX7_Technical_Analysis.html>. System
  overview, the 49,096 Hz rate, the logarithmic-storage rationale, the EGS
  frequency/envelope words, the DAC/compander description and the patent
  background.
- **Raph Levien, DX7 hardware notes** —
  <https://github.com/google/music-synthesizer-for-android/blob/master/wiki/Dx7Hardware.wiki>.
  The 12-bit `2^(v/256)` envelope (Q8 log₂) amplitude format.
- **Gambrell & Niemitalo, OPL3 decapsulation** —
  <http://yehar.com/blog/?p=665>. Comparison of a related Yamaha log-sine
  and exponent ROM, and the ROM read-out methodology.
- **Steffen Ohrendorf, OPL3 math document** —
  <https://raw.githubusercontent.com/gtaylormb/opl3_fpga/master/docs/opl3math/opl3math.pdf>.
  Formal description of the log→exp operator pipeline used to check the
  DX7 table recipes.
- **Acreil, DX7 research** — <https://acreil.wordpress.com/>. Harmonic
  images (`n·f` for `n = 4096`) and the assessment that the compander's own
  quantization is not a major source of the DX7's "grit".
- **Risto Holopainen, FM variants** —
  <https://ristoid.net/modular/fm_variants.html>. Background on iterated
  feedback and the "hunting" behavior behind the averaged feedback path.

## Service manuals, schematics and official documentation

- **Yamaha DX7/9 Service Manual** —
  <https://homepages.abdn.ac.uk/d.j.benson/pages/dx7/manuals/dx7-9_service_manual_1.pdf>
  (mirror: <http://www.synthmanuals.com/manuals/yamaha/dx7/service_manual/>).
  Clocking, the EGS rate-scaling buffer architecture, the algorithm
  walkthroughs, the DAC volume multiplexer and the output-filter sheet
  around `IC54`/`IC56`/`IC53`.
- **Yamaha DX7S Owner's Manual** (1987; local scan, no public URL pinned).
  The DX7S user-facing deltas: Poly/Mono/Unison Poly/Unison Mono (16/1/4/1
  notes), LFO Single/Multi with per-voice LFOs and Key Sync, PEG Range
  1/2, 1, 2, 8 octaves (1/2 = 6 semitones), PEG Velocity off/on and Rate
  Scaling 0–7, Unison Detune 0–7, Forced Damp continuation vs retrigger,
  plus the voice/performance/MIDI data tables.
- **Yamaha DX7S Complete Service Manual** —
  <https://archive.org/details/DX7S-SM>. Block diagram, IC block diagrams,
  parts list and overall circuit diagram for the DX7S board: `HD63B03YP`
  main CPU, `HD6805S1A33P` sub-CPU, `YM2604` OPS2 operators, `YM3609` EGM
  envelope/LFO, `PCM54HP` 16-bit DAC and `M58990P-1` ADC. Establishes that
  the DX7S is DX7II-class hardware, not the Mk1's YM21280/YM21290 +
  companded 12-bit output. The Overall Circuit Diagram (p. 40) also shows
  the post-DAC output chain: `PCM54HP` → 330 Ω → `74HC4066`
  sample-and-hold → buffered summing → a discrete multi-section
  3.3 kΩ/1.2 kΩ, 0.0082/0.015/0.001/0.068 µF low-pass, with `NJM4558DV`
  op-amps (no `TL072`). Those are the Mk1-class filter values, so the sheet
  does not support a distinct 20 kHz DX7S reconstruction corner; the exact
  response and the Mk1-vs-DX7S brightness cause remain open.
- **Yamaha TX7 Service Manual** — <https://archive.org/details/TX7SM>. The
  compander and `SF0–SF3` shift-data description, plus memory-map and
  interrupt flowcharts.
- **Yamaha DX7 schematics** — <https://archive.org/details/DX7SCHEMATICS>.
  BA9221 DAC, `IC41`/`IC46` switches, the R-2R scale ladder and the
  sample-and-hold/filter components.
- **Official Yamaha "DX7 Technical Analysis"** —
  <https://archive.org/details/yamaha-dx-7-technical-analysis>. Sound-chip
  and hardware documentation corroborating the die analyses.
- **Yamaha DX100 Owner's Manual** (scan mirror) —
  <https://synthfool.com/docs/Yamaha/DX_Series/Yamaha%20DX-100%20Owners%20Manual.pdf>.
  The DX100 user-facing surface: 4 operators, 8 algorithms, 8-voice poly,
  mono output, parameter ranges and order, the 64 fixed ratios, the
  AR/D1R/D1L/D2R/RR envelope, rate/level scaling, the four LFO waveforms and
  the fixed INIT VOICE. The scan omits the specification, MIDI and
  voice-data appendices.

## Patents

- **John M. Chowning, US 4,018,121 — "Method of synthesizing a
  musical sound"** — <https://patents.google.com/patent/US4018121A>.
  Original FM patent; the `sin{Kωt + f(ωmt)}` phase-addition formulation.
- **Norio Tomisawa, US 4,249,447 — "Tone production method for an
  electronic musical instrument"** —
  <https://patents.google.com/patent/US4249447A>. Operator self-feedback
  with the "anti-hunting" averaging of the last two samples.
- **Tetsuo Nishimoto, US 4,554,857 — "Electronic musical instrument
  capable of varying a tone synthesis operation algorithm"** —
  <https://patents.google.com/patent/US4554857A>. The
  operator box (phase modulation, log-domain `I(t)`/`A(t)` envelope) and
  the log frequency-number format.

## Firmware, ROM images and disassembly

- **ajxs, annotated DX7 v1.8 ROM disassembly** —
  <https://github.com/ajxs/yamaha_dx7_rom_disassembly>
  (raw listing:
  <https://raw.githubusercontent.com/ajxs/yamaha_dx7_rom_disassembly/master/yamaha_dx7_rom_v1.8.asm>).
  The EGS/OPS register map, the closed-form note→frequency law and per-key
  pitch curve, the shared 660-unit value scale, the pitch-EG composition and
  cadence, the LFO speed/delay laws and waveform recipes, the detune, bend,
  velocity, keyboard-scaling and amplitude-modulation tables, the
  pre-quantized EG registers, the velocity law, and the full verbatim table
  set.
- **YM2151 Application Manual**. The OPM register map, the eight `CON`
  algorithms, the AR/D1R/D1L/D2R/RR envelope laws and time tables, key/rate
  scaling, the LFO frequency and depth laws, `TL`/`MUL`/`DT1`/`DT2` semantics
  and the serial output format.

## Emulators and models (cross-checks)

- **Dexed / msfa** — <https://github.com/asb2m10/dexed>. The reference
  engine used for comparisons, and the source of hypothesis-grade models
  for the envelope, LFO, pitch-EG, frequency and algorithm tables. Its
  gaps (idealised ROM tables, no COM compensation, no DAC/compander) define
  several accuracy targets.
- **VDX7** The real v1.8 firmware driving emulated EGS/OPS at 49,096 Hz.
  Cross-checked the algorithm ROM, EGS register composition, fixed-operator
  behavior, the compander shift rule, the feedback division, the LFO/CPU
  cadence and the analog output-filter response.
- **picoX7** — <https://github.com/SloeComputers/picoX7>. A
  bit-synchronous Mk1 emulator used to confirm the envelope/phase
  architecture. It does not implement DX7 rate scaling.
- **Nuked-OPM** (cycle-accurate YM2151 model). Cross-checked the OPM register
  semantics, the exact EG increment arithmetic, `Rks = KC' >> (KS XOR 3)`,
  the `DT1` law, PMS/AMS scaling, the output frame saturation and feedback
  on M1; also supplied the chip-side DX100 algorithm anchors.
- **Plogue Chipsynth OPS7** —
  <https://www.plogue.com/products/chipsynth-ops7.html>. Reference-standard
  emulation used only for subjective comparison.

## Discussion and field reports

- **EE StackExchange, "How does the Yamaha DX7 digital-analog converter
  work?"** —
  <https://electronics.stackexchange.com/questions/512992/how-does-the-yamaha-dx7-digital-analog-converter-work>.
  Cross-check of the DAC and compander description.
- **Gearspace, "Is DX7 II quieter output than Mk1?"** —
  <https://gearspace.com/threads/is-dx7-ii-quieter-output-than-mk1.1443233/>.
  Field report: DX7 II (DX7S-class hardware with 16-bit PCM54HP DAC) has a
  lower noise floor and quieter output compared to the Mk1's companded
  12-bit output stage, consistent with the DX7S service manual's 16-bit DAC
  and Mk1-class output-filter components.
