# Changelog

## 0.2.0 (unreleased)

- A patch load now silences sounding voices, as the hardware's patch
  activation does; a key still held keeps its allocated voice but stops
  sounding until it is released and struck again.
- Evaluate the render path in operator-major blocks.
- Performance improvements include the use of `-O3` in the per-sample path,
  trimming of the output stage fixed cost, and removal of dead operator
  output storage.

## 0.1.0 (2026-11-18)

First working steps of the operator engine.

- The analog output stage is in: the hardware's reconstruction filter
  follows the master DAC step.
- The velocity route now follows the firmware's own law, matching its
  operator level register for operators without keyboard level scaling.
- The public API adds engine selection and error reporting. The DX7 is
  the default and only implemented engine; DX100 requests are rejected.
- The controllers implement the firmware's modulation arithmetic, routing
  and register recomposition.
- The LFO implements the firmware's speed and delay laws and six
  waveforms, updating its registers on every tick and resetting correctly
  on note-on.
- Amplitude modulation now follows the hardware's own reduction form.
- The LFO and pitch envelope cadences run off the same clock model.
- The render path now produces sound: patch loading, per-operator
  keyboard level scaling, the graph walk and the output stages.
- Voice management: 16-voice allocator with oldest-first stealing,
  sustain pedal, all-notes-off and firmware-accurate monophonic mode.
- The output stages implement the compander quantizer, mixed-bus sum,
  12-bit master DAC, master volume ladder and int16 conversion.
- The default master volume is now the hardware's reset value (level 7).
- The amplitude envelope follows the hardware's four-stage law and
  measured stage durations.
- The level pipeline implements the operator level curve, keyboard level
  scaling, velocity and carrier-count compensation.
- The operator core implements the phase accumulator, log-sine lookup,
  signal exponent and feedback path.
- The frequency path, pitch bend and pitch envelope follow the hardware's
  curves and quantization.
- The operator graph decodes the algorithm ROM into per-algorithm
  connection data.
- Table generators produce the oscillator, frequency and compensation
  tables, checked in and reproducible.
- Initial project skeleton: public header, context life cycle and API.
- Test harness with all stages and the `check`, `check-dx7`,
  `check-dx100` and `check-extras` targets.
- The validation loop compares renders against external reference
  renderers and writes `docs/validation.md`.
- Fixed-frequency detune now uses the same antisymmetric law as the ratio
  mode.
