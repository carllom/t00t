# T00T RP2350 Migration Plan

## Goal

Migrate from RP2040 (Cortex-M0+) to RP2350 (Cortex-M33) to exploit hardware
FPU, single-cycle SMULL, DSP/SIMD extensions, hardware divider, doubled SRAM,
and higher clock speed. Primary objective: **more simultaneous voices** — not
just better-sounding voices.

## Hardware

- **Board**: Pimoroni Pico VGA Demo Base (unchanged)
- **MCU**: RP2350 (dual Cortex-M33, 150 MHz, 520 KB SRAM)
- **DAC**: PCM5100A via I2S (DATA=GPIO26, BCK=GPIO27, LRCK=GPIO28, unchanged)
- **Buttons**: A=GPIO0, B=GPIO6, C=GPIO11 (active-high, pull-down, unchanged)
- **Profile pin**: GPIO2 (unchanged)

## RP2350 vs RP2040 — What We Gain

| Capability | RP2040 (M0+) | RP2350 (M33) | Synth Impact |
|---|---|---|---|
| Clock | 125 MHz | 150 MHz | ~20% free headroom |
| Multiply | MULS: 32×32→32 | SMULL: 32×32→64, 1 cycle | Full Q15×Q15→Q30, no overflow concern |
| FPU | None (soft float) | Single-precision, 1 cycle | Float envelopes/LFO, free modulation math |
| DSP | None | SMLAD, SMLADX, SMLSD, SMUAD, SMULBB etc. | Dual 16×16 MAC = 2 Q15 ops per instruction |
| SRAM | 264 KB | 520 KB | Larger voice arrays, bigger tables |
| Divide | Software (~50 cyc) | Hardware SDIV (2-12 cyc) | Free modulo in sample loops |
| Saturate | Manual clamp | SSAT/USAT instructions | Branchless clipping |

## Architecture Decisions

### Hybrid Q15 / Float Pipeline

Keep the hot inner loop in Q15 integer to leverage SIMD, use float where it
simplifies code without hurting throughput.

| Stage | Format | Rationale |
|---|---|---|
| Oscillator output | Q15 (int16_t) | Wavetable reads are naturally integer; BLEP corrections are integer |
| Amplitude scaling | Q15 via SMULL | `osc × velocity × envelope` → SMULL gives Q30 intermediate, shift once |
| SVF filter | Q15 state, Q15 coefficients | Hottest code: 4 multiplies/sample. SMLAD can pack two of these |
| Envelope compute | Float | One FPU op per sample per voice, no precision hacks, trivial code |
| Envelope output | Q15 (converted at point of use) | `(int16_t)(env_float * 32767.0f)` — single VCVT+multiply |
| LFO compute | Float | Sine via FPU sinf() or float table lookup. Rate as float Hz |
| LFO output | Q15 (converted at point of use) | Same conversion as envelope |
| Mix accumulator | int32_t | Unchanged — sum of Q15 voices, final clip to int16_t |

### What Becomes Moot

- **Inter-pass clamping in SVF**: With SMULL giving 64-bit intermediates, we can
  do Q15×Q15→Q30 and shift at end. No intermediate overflow.
- **Q14 vs Q13 resonance precision debate**: Use Q15 throughout, SMULL handles it.
- **`osc_phase_inc` avoiding float**: Float is free on Core 0, and even acceptable
  in render loop for non-critical paths.
- **`% loop_len` avoidance in sample.h**: Hardware divider makes modulo cheap.
- **Manual saturation branches**: SSAT instruction replaces all `if (s > 32767)` clamps.

### What Stays

- Dual-core split: Core 0 control / Core 1 render
- Double-buffered ParamExchange with volatile index flip
- DMA + PIO for I2S (same pins, same DAC)
- Voice allocation with bitmap feedback
- Module decomposition: oscillators, envelope, filter, voice_alloc, controller, output
- 44100 Hz sample rate, 256-sample buffers, stereo int16_t output

## Migration Phases

### Phase 1: Retarget and Smoke Test

Switch build to RP2350 and verify basic I2S output still works.

**Files touched:**
- `CMakeLists.txt` — add `set(PICO_PLATFORM rp2350)` or pass `-DPICO_BOARD=pico2`
- Verify pico-sdk 2.2.0 supports RP2350 (it does)
- `src/output.cpp` — PIO/DMA may need minor adjustments for RP2350 PIO
- Build, flash, verify sine wave output on DAC

**Done when:** Existing code builds and produces audio on RP2350 hardware.

### Phase 2: Rework Envelope to Float

Replace integer envelope with float internals. This is low-risk and
immediately simplifies code.

**Changes:**
- `src/envelope.h` / `src/envelope.cpp`:
  - `EnvConfig`: attack/decay/release rates as `float` coefficients (multiplicative)
  - `Envelope::level` → `float` (0.0–1.0)
  - `advance()` returns `float`, caller converts to Q15 at point of use
  - Attack: `level += attack_rate` (additive) or `level *= attack_coeff` (exponential — sounds better)
  - Decay/Release: `level *= decay_coeff` (exponential — natural amplitude decay)
- `src/audio_engine.cpp`: Convert envelope output to Q15 inline:
  `int32_t env_q15 = (int32_t)(envelope[v].advance(cfg) * 32767.0f);`

**Done when:** Envelope sounds correct, profile pin shows minimal change from Phase 1.

### Phase 3: Rework LFO to Float

**Changes:**
- `src/audio_engine.cpp`:
  - LFO phase accumulator → `float` (0.0–1.0, wrapping)
  - LFO value via `sinf()` or float sine table → avoid integer wavetable for LFO
  - Route to destinations as float, convert to Q15 only for oscillator/filter inputs
- `src/engine.h` / `VoiceParams`:
  - `lfo_rate` → `float` (Hz directly, no phase_inc encoding)
  - `lfo_depth` fields → `float` (natural units: semitones for pitch, Hz for filter, 0–1 for amplitude)
- Update controller to write float LFO params

**Done when:** LFO modulation sounds correct, profiling comparable.

### Phase 4: Rework SVF Filter with SMULL

This is the biggest performance win — the filter runs 4 multiplies per sample
per voice per pass, and is the hottest code.

**Changes:**
- `src/filter.h`:
  - Replace hand-clamped Q15 multiply with SMULL-based:
    ```
    int32_t q15_mul(int32_t a, int32_t b) {
        return (int32_t)(((int64_t)a * b) >> 15);
    }
    ```
  - Remove inter-pass clamping (SMULL can't overflow 64-bit intermediate)
  - Use SSAT for final output clamping (single instruction, branchless)
  - Coefficients: Q15 for both F and Q (no more Q14 compromise)
  - Consider: compute F_half as float, convert once per sample
- Evaluate SMLAD packing: pack two Q15 values in a 32-bit word, process
  two filter operations simultaneously (may require restructuring the 2-pass loop)

**Done when:** Filter sounds identical, profile pin shows reduction.

### Phase 5: Rework Amplitude Chain with SMULL

**Changes:**
- `src/audio_engine.cpp` inner loop:
  - Replace cascaded `(a * b) >> 15` with single SMULL chain:
    ```
    int32_t scaled = (int32_t)(((int64_t)osc * amplitude) >> 15);
    scaled = (int32_t)(((int64_t)scaled * env_q15) >> 15);
    ```
  - Evaluate combining into fewer operations
- Use SSAT for final clip instead of branch:
  `int16_t val = __SSAT(scratch[i], 16);`

**Done when:** Amplitude chain correct, no audible difference, profile improved.

### Phase 6: Increase Voice Count and Profile

**Changes:**
- `src/engine.h`: Increase `MAX_VOICES` (try 32, then 48, then 64)
- Profile with all voices active, find new ceiling
- Adjust `VoiceParamBlock` size (and total ParamExchange memory)
- Bitmap feedback may need `uint64_t` for >32 voices, or a second FIFO word

**Done when:** New MAX_VOICES established with measured profile data.

### Phase 7: Quality Improvements (Headroom Permitting)

Only if Phase 6 leaves headroom. These are stretch goals:

- **Cubic interpolation** for sample playback (Hermite 4-point, 4 multiplies)
- **Larger sine wavetable** (4096 entries, better high-frequency purity)
- **4-pole SVF option** (24dB/octave, cascaded 2-pole stages)
- **Per-voice envelope configuration** (different ADSR per button/patch)
- **Stereo panning** (per-voice L/R balance, trivial with current stereo buffer)

## Baseline Measurements (RP2040, Pre-Migration)

From profiling pin on GPIO 2 at 125 MHz:

| Scenario | Duty Cycle |
|---|---|
| Idle | 0.81% |
| Voice A: Fairlight sample (8-bit→Q15) + LP filter | 10.3% |
| Voice B: Square BLEP + LFO PWM + LP filter + env | 12.3% |
| Voice C: Triangle + LFO amplitude + LP filter | 10.5% |
| All 3 voices sustaining | 31.5% |
| Heavy use | Overloads buffer |

Target after migration: **at least 3× voice throughput** (same profile % per voice
at 150 MHz with SMULL/SIMD should yield ~4–6× improvement, but
float envelope/LFO adds some overhead back).

## Per-Phase Performance Tracking

Measured on GPIO 2 profiling pin. Same test scenarios at each phase to track
where the gains come from.

Test scenarios:
- **Idle**: No voices active
- **Voice A**: Fairlight sample (8-bit→Q15) + LP filter
- **Voice B**: Square BLEP + LFO PWM + LP filter + envelope modulation
- **Voice C**: Triangle + LFO amplitude + LP filter
- **All 3**: All three voices sustaining simultaneously

| Phase | Description | Idle | Voice A | Voice B | Voice C | All 3 |
|---|---|---|---|---|---|---|
| Baseline (RP2040 @ 125 MHz) | Original M0+ code | 0.81% | 10.3% | 12.3% | 10.5% | 31.5% |
| 1. Retarget RP2350 | Same code, M33 @ 150 MHz | | | | | |
| 2. Float envelope | Exponential env, FPU | | | | | |
| 3. Float LFO | Float LFO + natural units | | | | | |
| 4. SMULL filter | Q15×Q15→Q30, no clamping | | | | | |
| 5. SMULL amplitude | SMULL chain + SSAT clip | | | | | |
| 6. Voice count | MAX_VOICES increased | | | | | |
| 7. Quality | Stretch goals | | | | | |

## File Inventory

Files expected to change:

| File | Phase | Change |
|---|---|---|
| `CMakeLists.txt` | 1 | RP2350 platform target |
| `src/output.cpp` | 1 | Verify PIO/DMA on RP2350 |
| `src/envelope.h` | 2 | Float internals |
| `src/envelope.cpp` | 2 | Float internals |
| `src/audio_engine.cpp` | 2,3,4,5,6 | Incremental rework |
| `src/engine.h` | 3,6 | Float LFO params, MAX_VOICES |
| `src/filter.h` | 4 | SMULL-based SVF |
| `src/controller.h` | 3 | Float LFO params in ButtonState |
| `src/controller.cpp` | 3 | Float LFO param writing |
| `src/osc/sample.h` | 4 | Use hardware divider for loop wrap |
| `src/osc/common.h` | 3 | Possibly simplify phase_inc |
| `src/voice_alloc.h` | 6 | Wider bitmap if >32 voices |
| `src/voice_alloc.cpp` | 6 | Wider bitmap if >32 voices |
| `engine.md` | all | Update architecture docs |
