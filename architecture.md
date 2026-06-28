# T00T — Modular Engine Architecture (Design Draft)

This document captures the proposed direction for making t00t's synthesis pipeline
modular and swappable. It is a working design document — expect multiple revisions
before any code changes are made.

---

## Goals

- Swap the output generation engine at build time without touching the control plane.
- Keep the efficient dual-core, lock-free IPC structure.
- Allow fundamentally different synthesis methods: subtractive (current), aliased
  "chippy" waveforms, wavetable, FM, sample playback, speech, etc.
- Prefer more voices + slight fidelity cost over fewer voices + perfect audio.
- Open source; hobbyist / personal learning focus; not commercial.

Non-goals (for now):
- Running multiple synthesis engines simultaneously (see discussion below).
- Runtime engine switching without a reboot/reflash.

---

## Current Architecture (as-built)

```
Core 0 (control)                       Core 1 (audio)
────────────────────────────────────   ─────────────────────────────────────
USB MIDI  ─┐                           audio_engine_run()
DIN MIDI  ─┼→ midi_controller ──────┐    per-voice inner loop:
GPIO Btns ─┘   voice_alloc          │      envelope (ADSR)
                  allocate/release   │      LFO → pitch / PWM / amp / filter
                                     │      oscillator (dispatch by Waveform)
              ParamExchange   ───────┘      SVF filter (LP/BP/HP/notch)
              (double-buffer IPC)        mix → clip (__ssat) → I2S buffer
                  shadow / commit                  ↓
                                             DMA → PIO I2S → DAC
              ← active-voice bitmap ←── reverse FIFO (non-blocking)
```

### What is already modular

| Layer | Current state |
|-------|---------------|
| Input transports | USB MIDI and DIN MIDI are separate files; both funnel through `midi_controller_process()` |
| Voice allocator | Engine-agnostic: only tracks the 16-bit active-voice bitmap from Core 1 |
| I2S output | Self-contained in `output.h/cpp`; engines just fill an `int16_t` buffer |
| Envelope | `envelope.h/cpp` — reusable by any engine |
| SVF filter | `filter.h` — reusable |
| Oscillators | `osc/` — individual headers, already dispatched via `osc_sample()` |

### What is not yet modular

The synthesis engine (`audio_engine.cpp`) and the parameter struct (`VoiceParams` in
`engine.h`) are tightly coupled. `VoiceParams` carries subtractive-engine fields
(filter mode/cutoff/resonance, LFO depths, waveform, duty cycle, sample pointer)
that are meaningless for an FM or sample-only engine.

`VoicePreset` in `presets.h` mirrors this — it is also subtractive-specific.

---

## Proposed Layer Model

```
┌──────────────────────────────────────────────────────────────────────────┐
│  INPUT LAYER  (Core 0)                                                   │
│                                                                          │
│  USB MIDI ─┐                                                             │
│  DIN MIDI ─┼──→ midi_controller_process() ──→ voice_alloc_allocate()    │
│  GPIO Btns─┘                                   voice_alloc_release()    │
│  Sequencer (future) ────────────────────────→  (same entry points)      │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │  writes VoiceParams into shadow,
                                     │  calls params->commit()
                                     ▼
                          ParamExchange<VoiceParams>
                          (double-buffer, lock-free IPC)
                                     │
┌────────────────────────────────────▼─────────────────────────────────────┐
│  OUTPUT ENGINE  (Core 1)                                                 │
│                                                                          │
│  audio_engine_run(AudioBuffers*, ParamExchange*)                         │
│  ┌──────────────────────────────────────────────────────────────┐        │
│  │  Engine A: subtractive  (ADSR + LFO + SVF + osc dispatch)   │        │
│  │  Engine B: aliased      (raw waveforms, no filter, more vox) │        │
│  │  Engine C: wavetable    (ROM/flash table lookup)             │        │
│  │  Engine D: FM           (operator chains)                    │        │
│  │  Engine E: sample-only  (PCM playback, pitch-shifted)        │        │
│  └──────────────────────────────────────────────────────────────┘        │
│                      ↓                                                   │
│             DMA → PIO I2S → DAC                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

The **voice allocator** sits in the middle — it belongs to neither layer. It is
owned by Core 0, is engine-independent, and communicates with Core 1 only through
the reverse FIFO bitmap. No change needed there.

---

## The Coupling Problem: `VoiceParams`

`VoiceParams` is the only real cross-engine coupling point. Two design options:

### Option A — Compile-time per-engine `VoiceParams` (recommended)

Each engine defines its own `VoiceParams` that extends a shared base:

```cpp
// engine_base.h — stable across all engines
struct VoiceNoteBase {
    uint32_t phase_inc;   // pitch (pre-computed by Core 0)
    int16_t  amplitude;   // velocity (0–32767)
    uint8_t  trigger;     // generation counter, increment on note-on
    bool     gate;        // true = key held
};
```

The engine header extends this:

```cpp
// engines/subtractive/engine.h
struct VoiceParams : VoiceNoteBase {
    Waveform   waveform;
    uint16_t   duty_cycle;
    float      lfo_rate, lfo_depth, lfo_pitch_depth, lfo_pwm_depth, lfo_filter_depth;
    FilterMode filter_mode;
    uint16_t   filter_cutoff, filter_resonance;
    int16_t    filter_env_amount;
    const SampleDef *sample;
};

// engines/aliased/engine.h
struct VoiceParams : VoiceNoteBase {
    Waveform waveform;
    uint16_t duty_cycle;
    // no filter, no LFO — that's it
};

// engines/fm/engine.h
struct VoiceParams : VoiceNoteBase {
    struct Operator { uint32_t phase_inc; int16_t level; uint8_t feedback; };
    Operator ops[4];
    uint8_t algorithm;
};
```

`ParamExchange` is templated (or typedef'd) on the selected `VoiceParams`. The
CMake build includes exactly one engine directory, which defines both `VoiceParams`
and `audio_engine_run()`.

Pros: zero runtime overhead; each engine's param block is exactly the size it needs;
inner loop sees concrete types, enabling full compiler optimisation.

Cons: Core 0's controller and preset code must also be engine-aware (see below).

### Option B — Tagged union `VoiceParams`

One large struct with a `EngineType` tag and a `union` payload. Allows per-voice
engine type at runtime (e.g., voice 0 FM, voice 1 subtractive).

Pros: simultaneous mixed engines; no recompile to switch sound.

Cons: union is sized to the largest engine's params; per-sample dispatch switch
in the inner loop; more complex Core 0 preset management.

**Conclusion:** Start with Option A (compile-time). The aliased engine being a
first-class alternative already covers the main goal. Option B can be revisited if
mixed-engine polyphony becomes a priority.

---

## Simultaneous Engines — Discussion

Running FM voices alongside subtractive voices in one render pass requires Option B
above, plus a per-voice type dispatch in the inner loop (one switch per sample ×
voice). At 16 voices and ~75% Core 1 utilisation already, there is limited headroom.

More practical split: use a build-time engine that is internally multi-timbral by
waveform type (the current engine already dispatches oscillator type per voice).
For *fundamentally* different synthesis methods (FM vs. subtractive), one engine
per build keeps the code clean and the performance predictable.

If simultaneous engines are desired later, the tagged-union path (Option B) is the
right extension. The base struct design from Option A makes the migration easier.

---

## Proposed Directory Layout

```
src/
  engine_base.h            ← VoiceNoteBase, ParamExchange, MAX_VOICES, PROFILE_PIN
  audio_common.h           ← SAMPLE_RATE, SAMPLES_PER_BUFFER (unchanged)
  output.h / output.cpp    ← I2S DMA output (unchanged)
  voice_alloc.h/.cpp       ← voice allocator (unchanged)
  envelope.h/.cpp          ← ADSR (reusable, unchanged)
  filter.h                 ← SVF (reusable, unchanged)
  osc/                     ← oscillator modules (reusable, unchanged)
  midi/                    ← MIDI transports (unchanged)
  controller.h/.cpp        ← GPIO buttons (unchanged)

  engines/
    subtractive/
      engine.h             ← VoiceParams + audio_engine_run() declaration
      engine.cpp           ← current audio_engine.cpp, lightly moved
      presets.h            ← VoicePreset + preset table (subtractive-specific)
    aliased/
      engine.h             ← minimal VoiceParams (no filter/LFO fields)
      engine.cpp           ← stripped render loop, higher voice budget
      presets.h
    wavetable/             ← (future)
    fm/                    ← (future)
    sample/                ← (future)

  main.cpp                 ← includes engine via ENGINE_DIR/engine.h
```

CMake selects the engine:

```cmake
set(T00T_ENGINE "subtractive" CACHE STRING "Synthesis engine")
# values: subtractive | aliased | wavetable | fm | sample

target_include_directories(t00t PRIVATE
    src/engines/${T00T_ENGINE}
    src
)
```

`main.cpp` includes `engine.h` (no path prefix) and gets whichever engine was
selected. Same for `presets.h`.

---

## Core 0 Coupling to Engine

The controller and MIDI code on Core 0 must write into `VoiceParams`. Today this
is done via `voice_apply_preset()` in `presets.h`, which copies a `VoicePreset`
into a `VoiceParams`. Since `VoicePreset` mirrors `VoiceParams` fields, it is
also engine-specific.

Each engine therefore ships its own `presets.h` defining:
- `VoicePreset` (engine-specific fields)
- `voice_apply_preset(VoiceParams&, const VoicePreset&)`
- `PresetId` enum
- The preset table itself

The MIDI controller and button controller reference presets by index (`PresetId`),
so their logic is unchanged. Only the preset data and the apply function are
engine-specific. The MIDI controller itself (`midi_controller.cpp`) does not need
to change as long as it writes only `VoiceNoteBase` fields (trigger, gate,
phase_inc, amplitude) and delegates timbre to a preset — which is already the case.

---

## The Aliased Engine — Concrete First Target

As a practical first alternative engine, "aliased" removes the expensive parts of
the subtractive engine:

| Feature | Subtractive | Aliased |
|---------|-------------|---------|
| Oscillators | All (BLEP + raw) | Raw only (square/saw/triangle/noise/sine) |
| Envelope | Per-preset ADSR | Per-preset ADSR (same — cheap and important) |
| LFO waveforms | Sine/triangle/square/saw/S&H, 4 destinations | Amplitude-only (tremolo), optional |
| SVF filter | Yes | No |
| Sample playback | Yes | No (separate sample engine if needed) |
| Estimated cost/voice | ~5–7% | ~2–3% |
| Voices at 75% budget | 10–14 | 25–35 |

The aliased engine's `VoiceParams`:

```cpp
struct VoiceParams : VoiceNoteBase {
    EnvConfig env;        // per-preset ADSR (pre-computed by Core 0)
    Waveform  waveform;   // WAVE_SINE/SQUARE/TRIANGLE/SAW/NOISE only
    uint16_t  duty_cycle;
    // optional tremolo LFO — add if budget allows
    float     lfo_rate;
    float     lfo_depth;  // 0 = off
};
```

The render loop is the current inner loop stripped to: advance envelope, compute
oscillator sample, scale by amplitude × envelope × optional tremolo, accumulate.
No filter, no pitch/PWM/filter LFO, no per-sample coefficient computation. The
inner loop becomes ~6–8 ops per sample per voice vs. ~30+ for subtractive.

Intentional aliasing at high notes is a feature here — think NES/Game Boy square
waves, not a defect to be corrected.

---

## Input Layer: Adding a Sequencer / Mod Source

A step sequencer or modulation source would sit on Core 0 and call the same
`voice_alloc_allocate()` / `voice_alloc_release()` that MIDI and buttons use.
No structural change to the allocator, IPC, or engine needed.

The main loop currently calls input sources explicitly:

```cpp
usb_midi_task(); usb_midi_poll(&params);
uart_midi_poll(&params);
controller_tick(&params);   // HAS_BUTTONS
```

A sequencer becomes another call in this list:

```cpp
sequencer_tick(&params);    // HAS_SEQUENCER
```

The sequencer would maintain its own step state and timing, generate note events,
and drive the voice allocator. It could share the same preset table.

---

## Inter-Core IPC: No Changes Needed

The `ParamExchange` double-buffer mechanism is sound and should not change. The
only structural difference with a templated `VoiceParams` is size:

```cpp
template<typename VP>
struct ParamExchange {
    struct Block { VP voices[MAX_VOICES]; };
    Block blocks[2];
    volatile uint8_t committed;
    // ... same shadow/commit/active methods
};
```

Or equivalently, a preprocessor typedef if templates complicate the C linkage
in certain places. The memory barrier + `__sev()` commit dance is unchanged.

The reverse FIFO (active-voice bitmap) is unchanged: always a `uint32_t` bitmask,
engine-agnostic.

---

## Settled Decisions

### ADSR is per-preset

ADSR parameters move out of `audio_engine_run()` and into `VoicePreset`. Each
preset carries its own envelope shape. `voice_apply_preset()` pre-computes the
`EnvConfig` (coefficients, not raw ms values) and stores it in `VoiceParams` so
Core 1 sees only the pre-baked values and pays no conversion cost per render pass.

```cpp
// VoicePreset gains:
uint16_t attack_ms;
uint16_t decay_ms;
uint8_t  sustain_pct;   // 0–100
uint16_t release_ms;

// voice_apply_preset() calls env_config() and stores result:
vp.env = env_config(pr.attack_ms, pr.decay_ms, pr.sustain_pct, pr.release_ms);

// VoiceParams gains (all engines):
EnvConfig env;
```

The hardcoded `env_cfg` local in `audio_engine_run()` is removed; each voice reads
`p.env` directly. No runtime overhead increase — `EnvConfig` is four floats, the
same computation that was previously done once globally is now done once per
note-on (in Core 0, inside `voice_apply_preset()`).

### LFO waveforms via lookup tables (subtractive engine; optional in others)

The LFO waveform becomes a `VoicePreset` field for engines that support modulation.
Core 1 dispatches to the correct table or generator per voice, same pattern as
oscillator waveform dispatch today.

Proposed LFO waveforms:

```cpp
enum LfoWaveform : uint8_t {
    LFO_SINE,        // current behaviour — smooth vibrato/tremolo
    LFO_TRIANGLE,    // linear ramp up/down
    LFO_SQUARE,      // hard tremolo, trill effect
    LFO_SAW,         // rising ramp — one-shot sweep when rate is slow
    LFO_SAMPLE_HOLD, // random stepped value, updated at lfo_rate
};
```

Sine and triangle share the existing wavetable (triangle is just a different read
pattern or a second small table). Square and S&H are trivial. All LFO types are
Q15 integer output — the rest of the LFO chain is unchanged.

Not all engines need to implement all LFO types. The aliased engine may carry a
simplified amplitude-only LFO (tremolo) if the CPU budget allows; it is not
required to support pitch or filter modulation.

### Core 1 → Core 0 feedback: active-voice bitmap is sufficient

The existing 16-bit voice-active bitmap pushed via the reverse FIFO covers the
allocator's needs. The allocator only needs to know when a voice has gone fully
silent so it can be reclaimed — not the current envelope level. No richer feedback
channel is needed at this stage.

---

## Open Questions

1. **Stereo panning**: The output is currently mono-duplicated to L+R. A pan field
   in `VoiceNoteBase` (or engine-level) would allow voice-level stereo spread
   with trivial cost (multiply by a Q15 pan coefficient at the output stage).
   Worth adding to the base struct now while the interface is being redesigned, or
   leave as a future engine option?

2. **Engine versioning / incompatibility guard**: If `VoiceParams` is
   engine-specific, the compiled firmware should refuse to run if the wrong
   preset is applied. A `static_assert` on param struct size, or a compile-time
   `ENGINE_ID` constant checked at init, would catch mismatches during development.

3. **Simultaneous engines**: If the aliased engine gives 30+ cheap voices, one
   potential use is a hybrid: a few "expensive" subtractive voices for lead + many
   aliased voices for chords. This is the Option B (tagged union) case. Worth
   noting but not designing for now.

---

## Next Steps (not code — decisions first)

- [x] Agree on Option A (compile-time) as the starting approach
- [x] ADSR moves into `VoicePreset` as ms values; `EnvConfig` stored in `VoiceParams`
- [x] LFO waveform selection in subtractive engine; optional/limited in aliased
- [x] Core 1 feedback stays as active-voice bitmap — no richer channel needed
- [ ] Resolve open question 1 (stereo panning in base struct or not)
- [ ] Sketch the aliased engine's full `VoiceParams` and preset table
- [ ] Confirm directory layout, then: move code into `engines/subtractive/`, template `ParamExchange`, build aliased engine
