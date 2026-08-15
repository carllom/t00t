#pragma once

#include <cstdint>
#include "sid_osc.h"
#include "env_sid.h"
#include "sid_tables.h"

// One SID voice: oscillator x envelope, through the two DACs.
//
// TOPOLOGY-FREE (module_chip.md §4). No bus index, no voice index, no neighbour. Sync
// and ring arrive as values the caller computed; where they came from is the
// caller's business, which is what lets CHIP_STRICT's adjacency wiring and the
// engine's per-voice sub-oscillator (§4.4) share this code.
//
// ---------------------------------------------------------------------------
// THE FIXED-POINT CONTRACT
//
// One anchor, stated once, with everything else expressed beneath it:
//
//     voice output = (waveform_dac12(wave) - wave_zero) * envelope_dac8(env)
//
// which is reSID's own Voice::output(), range [-2048*255, 2047*255].
// Adopting the reference's scale rather than inventing one means there is
// exactly one scale, and it is the reference's, so any level disagreement
// with reSID is a bug in a specific curve rather than a tuning opportunity.
// ---------------------------------------------------------------------------

// The 12-bit waveform DAC is 8 KB of flash (sid_tables.h). The 8-bit envelope
// DAC is 256 bytes and is always on -- at low envelope levels the 6581's
// missing bit-0 termination is most of what makes a quiet note sound like a
// 6581 rather than like an attenuated square wave.
//
// Default on, so the default build is the accurate one and turning it off
// is a deliberate act with a number attached (tools/sid_compare.py reports
// the difference).
#ifndef CHIP_WAVE_DAC
#define CHIP_WAVE_DAC 1
#endif

// Velocity scaling is applied *before* the bus sum, not after the filter
// (module_chip.md §13.7): a quiet note then drives the 6581 nonlinearity
// less hard. It compiles out entirely under CHIP_STRICT (§11.1), so the
// reSID diff stays isolated to the primitives.
#ifndef CHIP_STRICT
#define CHIP_STRICT 0
#endif

struct SidVoice {
    SidOsc osc;
    EnvSid env;
    uint8_t model;       // SidFilterModel: selects wave_zero and the DAC tables
#if !CHIP_STRICT
    uint8_t velocity;    // 1..127, 0 treated as 127
#endif

    void init(uint8_t chip_model = 0) {
        osc.init();
        env.init();
        model = chip_model;
#if !CHIP_STRICT
        velocity = 127;
#endif
    }

    inline int32_t wave_zero() const {
        return model ? SID_WAVE_ZERO_8580 : SID_WAVE_ZERO_6581;
    }

    // Advance one sample. Returns the voice output in the contract's units.
    inline int32_t tick(const EnvSidRates &rates, bool sync_reset, uint16_t ring_msb_flip) {
        osc.advance();
        if (sync_reset) osc.sync();
        return output_and_tick_env(rates, ring_msb_flip);
    }

    // The output half, for callers that drove osc.advance() themselves so that
    // sync could be resolved across voices first (see sid_osc.h's phase note).
    //
    // Named for the side effect rather than the return value: this ticks the
    // envelope. Calling it twice for one sample advances the envelope twice,
    // which is a quiet 2x-fast ADSR rather than an error -- exactly the kind of
    // bug the control-plane diff would report as a wrong rate table and send
    // someone looking in the wrong file.
    inline int32_t output_and_tick_env(const EnvSidRates &rates, uint16_t ring_msb_flip) {
        uint16_t wave = osc.output(ring_msb_flip);
        uint8_t  e    = env.tick(rates);

#if CHIP_WAVE_DAC
        int32_t w = model ? (int32_t)wave : (int32_t)SID_WAVE_DAC_6581[wave];
#else
        int32_t w = (int32_t)wave;
#endif
        int32_t amp = model ? (int32_t)SID_ENV_DAC_8580[e] : (int32_t)SID_ENV_DAC_6581[e];

        int32_t out = (w - wave_zero()) * amp;

#if !CHIP_STRICT
        // Digital scaling, ahead of any filter.
        uint8_t v = velocity ? velocity : 127;
        if (v != 127) out = (int32_t)(((int64_t)out * v) >> 7);
#endif
        return out;
    }
};

// Peak magnitude of one voice under the contract, for headroom arithmetic.
// The 6581 is asymmetric because its waveform DAC zero is 0x380, not 0x800
// (sid_osc.h) -- so a full-scale sawtooth swings +3199/-896, not +-2048.
static constexpr int32_t SID_VOICE_PEAK_POS_6581 = (0xfff - SID_WAVE_ZERO_6581) * 255;
static constexpr int32_t SID_VOICE_PEAK_NEG_6581 = SID_WAVE_ZERO_6581 * 255;
