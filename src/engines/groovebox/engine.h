#pragma once

#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"
#include "envelope.h"       // EnvConfig (pre-baked per note by Core 0)

// Groovebox engine — a TB-303-style acid bass plus an 808/909-style drum
// machine sharing one 16-voice render pass. Unlike the subtractive engine
// (all voices identical, dispatched by waveform), voices here are
// heterogeneous: each carries a VoiceType and the render loop dispatches per
// voice. See module_groovebox.md for the full design.
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
    int16_t   pan;       // Q15 pan: -32768 = full left, 0 = center, 32767 = full right

    uint32_t  phase_inc;   // primary oscillator pitch (303 / BD / tom / snare tone 1)
    uint32_t  phase_inc2;  // secondary oscillator pitch (snare tone 2)

    Waveform  waveform;    // primary osc for 303 (WAVE_SAW / WAVE_SQUARE)
    uint16_t  duty_cycle;  // square duty (0..1023, 512 = 50%)

    float     drive;       // 303: ladder input overdrive (1.0 = clean; accent adds)

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

// Default voice is zero-init — VoiceType 0 is VT_SILENT, so this is the
// generic engine_base.h default already. No specialization needed.

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
