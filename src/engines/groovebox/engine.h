#pragma once

#include "engine_base.h"
#include "envelope.h"       // EnvConfig (pre-baked per note by Core 0)
#include "hardware/sync.h"
#include <cstdint>

// Groovebox engine — a TB-303-style acid bass plus an 808/909-style drum
// machine sharing one 16-voice render pass. Unlike the subtractive engine
// (all voices identical, dispatched by waveform), voices here are
// heterogeneous: each carries a VoiceType and the render loop dispatches per
// voice. See groovebox.md for the full design.
//
// Voice slots are assigned statically by the controller (fixed drum map + a
// mono 303), not by the dynamic allocator.

enum VoiceType : uint8_t {
    VT_SILENT = 0,   // nothing (default) — skipped by the render loop
    VT_TB303,        // saw/square + resonant LP + amp ADSR + filter decay env
    VT_DRUM_BD,      // sine + downward pitch env + amp decay
    VT_DRUM_TOM,     // sine + pitch env (lo/mid/hi via tune) — BD generator, retuned
    VT_DRUM_SNARE,   // two shell tones + noise -> band-pass
    VT_DRUM_HAT,     // noise -> high-pass + decay (closed/open via decay time)
    VT_DRUM_METAL,   // six-square metal bank -> band-pass -> high-pass (808 hats/cymbal)
    VT_DRUM_CLAP,    // band-passed noise + multi-burst clap envelope
    // Future: VT_DRUM_SAMPLE (909).
};

// Per-voice parameters. Written by Core 0, read by Core 1. A flat struct
// (rather than a union) — with only 16 voices the few extra bytes are free and
// the field-by-field init is far less error-prone. Unused fields per type are
// simply ignored by that type's render path.
struct VoiceParams {
    VoiceType type;      // instrument type (VT_SILENT = silent)
    uint8_t   trigger;   // generation counter, ++ on each note-on
    bool      gate;      // held (303); one-shot drums ignore it
    bool      slide;     // 303: glide pitch toward phase_inc instead of snapping
    int16_t   amplitude; // velocity 0..32767

    uint32_t  phase_inc;   // primary oscillator pitch (303 / BD / tom / snare tone 1)
    uint32_t  phase_inc2;  // secondary oscillator pitch (snare tone 2)

    Waveform  waveform;    // primary osc for 303 (WAVE_SAW / WAVE_SQUARE)
    uint16_t  duty_cycle;  // square duty (0..1023, 512 = 50%)

    EnvConfig amp_env;     // amplitude contour (ADSR for 303, one-shot decay for drums)
    EnvConfig aux_env;     // one-shot decay: filter env (303) OR pitch env (drums)

    // Filter (303, hat, snare)
    FilterMode filter_mode;     // LP (303) / HP (hat) / BP (snare) / OFF
    uint16_t   filter_cutoff;   // base cutoff Hz (303/snare/hat); BP center (metal)
    uint16_t   filter_cutoff2;  // second corner Hz — HP after the BP (metal only)
    uint16_t   filter_resonance;// 0..32767 (0 = none, 32767 = self-oscillation)
    int16_t    filter_env_amount;// aux_env -> cutoff Hz (303 env mod, signed)

    // Drums
    int16_t    pitch_env_depth; // aux_env -> pitch, Q15 fraction of base (BD/tom/snare)
    uint16_t   noise_level;     // Q15 noise mix (snare/hat)
    uint16_t   tone_level;      // Q15 tone mix (snare)
    uint8_t    metal_first;     // first metal-osc index (metal voices)
    uint8_t    metal_count;     // number of metal oscillators to sum (2 = cowbell, 6 = hats)
};

// A complete snapshot of all voice parameters for one render pass.
struct VoiceParamBlock {
    VoiceParams voices[MAX_VOICES];
    EffectParams fx;
};

// Double-buffered parameter exchange between Core 0 and Core 1.
// Same lock-free mechanism as the subtractive engine — only the payload
// differs. Core 0 writes the shadow, flips committed, __sev()s Core 1.
struct ParamExchange {
    VoiceParamBlock blocks[2];
    volatile uint8_t committed;  // 0 or 1

    void init() {
        committed = 0;
        for (int b = 0; b < 2; b++) {
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                blocks[b].voices[v] = VoiceParams{};  // zero → VT_SILENT
            }
            // Default: delay selected, ~300 ms / moderate feedback, fully dry
            // (mix=0) so it's silent until CC73 opens it. Matches subtractive.
            blocks[b].fx = { FX_DELAY, 0, 55, 36 };
        }
    }

    VoiceParamBlock &shadow() { return blocks[1 - committed]; }

    void commit() {
        __compiler_memory_barrier();
        committed = 1 - committed;
        __sev();
    }

    const VoiceParamBlock &active() const { return blocks[committed]; }
};
