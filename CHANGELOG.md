# Changelog

## 0.3.0 (unreleased)

Add experimental DX7S personality.

- Add experimental, unverified DX7S personality.
- Add DX7S-only voice parameter setters, gated by personality.
- Implement the DX7S pitch EG's range, velocity switch and rate scaling.
- Add DX7S unison modes: fixed unison groups, oldest-group-first
  stealing with envelope continuation, per-group pedal and all-notes-off,
  and a provisional unison detune spread (neutral by default).
- Fix the DX7S unison detune spread: the rendered offsets now round onto
  whole EGS frequency-field units, the same grid the per-operator detune
  steps on, instead of the grid's floor division rendering the stack
  biased flat-downward with half its spread rounded away.
- Fix DX7S key-mode switching: the sounding voices re-pitch at once when
  the switch drops their unison offsets, and the keys the old mode's
  bookkeeping owned stay releasable - the unison note-off/all-notes-off
  paths free orphaned held voices through a per-voice pass, and a switch
  into mono seeds its single-key bookkeeping from voices[0] and frees
  the held voices mono cannot own (the pedal defers these releases).
  Re-applying the current mode is a no-op, so it cannot clear a live
  stack's groups or flatten its detune.
- Synchronize the DX7S's `slopsix_set_mono_mode` with its key mode: the
  DX7S has no separate mono switch, so the setter now maps onto the key
  mode (mono-on = Mono, mono-off = Poly) and reuses the key-mode switch's
  re-homing. Previously a Mono key mode under `set_mono_mode(false)`
  allocated the polyphonic path and a Poly key mode under
   `set_mono_mode(true)` allocated the mono one.
- Reset the stolen stack's LFO state on the DX7S: a key event that
  continues the envelopes (the Forced Damp Off steal) now also runs the
  LFO note-on resets, re-seeding the keyed voice's sync'd phase and
  clearing its patch delay's fade pair, like the fresh trigger does.
- Record the DX7S LFO mode transition model: entering Multi re-seeds all
  sixteen voices from the Single engine (a Single/Multi round trip resets
  the voices' independent histories), and the Single engine stays parked
  through Multi and resumes untouched on the return. Document-only
  provisionally pinned by the harness (OQ-DX7S-01); no behaviour change.
- Correct the controller-recompose documentation: a routed controller's
  event recomposes the pitch-modification register at once, but the
  sounding voices re-pitch at the next LFO tick, where the firmware's
  per-tick register push is the engine's rewrite gate (the bend's own
  setters refresh immediately; both are deliberate). Pinned by the
  controller stage's checks.
- Implement portamento in the DX7S unison modes: the key events take the
  polyphonic composition, so with the pedal a fresh group glides from the
  instrument's last target and a stolen stack glides out of its own
  buffer (full time mode drags pedal-held and released voices along);
  without the pedal the buffers snap. The unison paths previously snapped
  every buffer, making portamento and glissando inert there.
- Set the DX7's analog output tail to the measured 17.02 kHz -3 dB
  corner, the DX7S tail is set to 20 kHz.
- Give the output stage one bit (6 dB) of headroom, matching the VDX7
  reference level. Full-level patches no longer saturate at the final
  conversion when the analog tail overshoots.
- Stop clamping the mixed voice bus at a single voice's 12-bit DAC
  window. The per-voice compander already owns that window and the bus is
  the analog sum, so loud chords no longer hard-clip at one voice's level;
  only the final int16 conversion (the analog stage's rail) saturates.
- Remove DX100 support scaffolding.

## 0.2.0 (2026-11-19)

Performance optimization and output gain control.

- Add output volume control (`slopsix_set_output_gain`) and rename
  `slopsix_set_master_volume` to `slopsix_set_midi_volume`.
- Silence sounding voices when a patch is loaded.
- Add a Q16 host output gain (`slopsix_set_output_gain`), composed
  with the hardware master-volume ladder in the final conversion's cached
  gain so the sample path gains no multiply.
- Implement portamento and glissando.
- Evaluate the render path in operator-major blocks.
- Performance improvements: use `-O3` in the per-sample path, trim output
  stage fixed cost, and removal of dead operator output storage.
- Compile the algorithm schedule and block renderer at `-O2`: this keeps
  the archive at about 56 KB instead of 70 KB, with the render within a
  few percent of the all `-O3` build.
- Fix the sample-rate rescale: the phase increment is now scaled by the
  anchor rate divided by the host rate, so the pitch no longer follows the
  rate squared on hosts other than 49,096 Hz. Very low host rates saturate
  the increment instead of wrapping it.

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
