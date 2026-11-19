# Slopsix

A dependency-free, 32-bit friendly, integer-only, fixed-point FM
synthesis library that emulates the DX7 sound generator, with optional
extras behind compile-time feature macros.

The library renders into caller-supplied buffers, performs no I/O, and
allocates only when a context is created. It aims to be more accurate
than Dexed, reproducing the hardware's tables, curves and quirks.

## Building

```sh
make            # libslopsix.a and the test binary
make check      # regenerate-and-compare the tables, then run the harness
make example    # OpenAL terminal keyboard player
make validate-render  # the slopsix side of the validation loop
make validate BANKS="bank1.syx bank2.syx"  # render and compare
make clean
```

Requirements: a C99 compiler, `ar`, and Python 3 for the table
generators. The library itself is integer-only — the build enables
`-Wfloat-conversion -Wdouble-promotion` where the compiler supports them,
so a floating-point operation anywhere in the library is a build error.

Everything is compiled size-first (`-Os`) except the translation units on
the per-sample render path, which are compiled speed-first (`-O3`).

## Public API

`include/slopsix.h` is the only public header. The essentials:

```c
slopsix_context ctx = slopsix_create_context(SLOPSIX_ENGINE_DX7, 49096);
slopsix_set_voice(ctx, voice_dump_156, 156);
slopsix_note_on(ctx, 60, 100);
slopsix_process(ctx, stereo_buffer, frames);
slopsix_note_off(ctx, 60);
slopsix_destroy_context(ctx);
```

Units and conventions:

- human-facing scalars are **Q16** — `1.0 == 65536` is one unit of the
  parameter's own unit (gain, pan position, cents);
- parameters whose value the hardware itself encodes keep that integer's
  format (bend 14-bit, controller 0…127, master volume 0…7);
- the engine's internal log-frequency word uses the hardware's own
  4096-units-per-octave domain; the chip's field is that word shifted
  down.

The API carries the settled shape (plan decisions D-7 and D-17…D-21).
Every note event and setter returns a `slopsix_error`.

Context and render:

```c
slopsix_context slopsix_create_context(slopsix_engine engine,
                                       uint32_t sample_rate);
void slopsix_destroy_context(slopsix_context ctx);
void slopsix_set_sample_rate(slopsix_context ctx, uint32_t sample_rate);
void slopsix_reset(slopsix_context ctx);
void slopsix_process(slopsix_context ctx, int16_t *stereo_out, size_t frames);
```

Notes and voice management:

```c
slopsix_error slopsix_note_on(slopsix_context ctx, uint8_t note,
                              uint8_t velocity);
slopsix_error slopsix_note_off(slopsix_context ctx, uint8_t note);
slopsix_error slopsix_all_notes_off(slopsix_context ctx);
slopsix_error slopsix_set_sustain(slopsix_context ctx, bool on);
slopsix_error slopsix_set_mono_mode(slopsix_context ctx, bool mono);
```

Portamento (`SLOPSIX_PORTA_FINGERED` or `SLOPSIX_PORTA_FULL_TIME`) and
glissando:

```c
slopsix_error slopsix_set_portamento_time(slopsix_context ctx,
                                          uint8_t time_0_99);
slopsix_error slopsix_set_portamento_mode(slopsix_context ctx,
                                          slopsix_portamento_mode mode);
slopsix_error slopsix_set_portamento_pedal(slopsix_context ctx, bool on);
slopsix_error slopsix_set_glissando(slopsix_context ctx, bool on);
```

Pitch and tuning:

```c
slopsix_error slopsix_set_pitch_bend(slopsix_context ctx, uint16_t bend);
slopsix_error slopsix_set_pitch_bend_range(slopsix_context ctx,
                                           uint8_t range_semitones,
                                           uint8_t step);
slopsix_error slopsix_set_master_tune(slopsix_context ctx, int32_t cents_q16);
```

Controllers (`SLOPSIX_CTRL_MOD_WHEEL`, `FOOT`, `BREATH`, `AFTERTOUCH`):

```c
slopsix_error slopsix_set_ctrl_value(slopsix_context ctx,
                                     slopsix_ctrl_source src,
                                     uint8_t value_0_127);
slopsix_error slopsix_set_ctrl_range(slopsix_context ctx,
                                     slopsix_ctrl_source src,
                                     uint8_t range_0_99);
slopsix_error slopsix_set_ctrl_assign(slopsix_context ctx,
                                      slopsix_ctrl_source src,
                                      unsigned flags);
```

with `SLOPSIX_ASSIGN_PITCH`, `SLOPSIX_ASSIGN_AMP` and `SLOPSIX_ASSIGN_EG`
OR-ed into `flags`.

Patches (one live voice; no bank or program-change API):

```c
slopsix_error slopsix_set_voice(slopsix_context ctx, const uint8_t *data,
                                size_t size);
slopsix_error slopsix_get_voice(const slopsix_context ctx, uint8_t *out,
                                size_t size);
```

Output:

```c
slopsix_error slopsix_set_pan(slopsix_context ctx, int32_t pan_q16);
slopsix_error slopsix_set_master_volume(slopsix_context ctx,
                                        unsigned level_0_7);
```

Capability queries: `slopsix_context_engine`, `slopsix_voice_count`,
`slopsix_operator_count`, `slopsix_algorithm_count`,
`slopsix_voice_dump_size`, `slopsix_context_sample_rate`.

- the **engine** (personality) is chosen at context creation and never
  changes; the render entry point is resolved there and stored in the
  context, so `slopsix_process()` is one indirect call per buffer with no
  personality branch in the sample loop;
- the **feature macros** decide which engines are compiled in; asking for
  an engine whose macro is off fails at creation;
- the setters **return an error**, so a call shared by multiple engines can
  report that one of them lacks the feature (`SLOPSIX_ERR_UNSUPPORTED`);
- the patch I/O is **size-parameterised**: 156 bytes for the DX7,
  validated against the engine's own size;
- the engine's shape is queryable (`slopsix_context_engine`,
  `slopsix_voice_count`, `slopsix_operator_count`,
  `slopsix_algorithm_count`, `slopsix_voice_dump_size`), so host code can
  be written once across engines;
- panning is an **extra**, not hardware behaviour: every supported
  instrument is mono, and the default centre leaves the faithful signal in
  both channels.  `slopsix_set_pan` is available behind `SLOPSIX_EXTRAS`;
  without it the call is a no-op, the output stays at centre and the call
  reports `SLOPSIX_ERR_UNSUPPORTED`;
- every setter and every note event **returns an error code**: `SLOPSIX_OK`,
  `SLOPSIX_ERR_RANGE` (a value was out of range and was clamped),
  `SLOPSIX_ERR_UNSUPPORTED` (the engine does not have the feature; the
  value is left unchanged) or `SLOPSIX_ERR_INVALID` (a malformed call).

## Example

`make example` builds `build/<config>/examples/play_keys`, a terminal
keyboard player that streams the engine through OpenAL. It starts with a
small built-in voice; keys toggle notes (the terminal has no key-release
event), `[`/`]` transpose the octave, Space releases everything, and Q or
Esc quits.

## Scope

The library implements the instrument's sound path. Patch storage, MIDI
parsing, SYSEX framing, panel and LCD emulation are out of scope.
