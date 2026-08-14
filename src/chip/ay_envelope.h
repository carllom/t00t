#pragma once

#include <cstdint>
#include "ay_osc.h"

// AY-3-8910/YM2149 envelope generator + DAC tables.
//
// TOPOLOGY-FREE (see ay_osc.h's header comment -- same file, same rule).
//
// chip.md §12: "envelope generator at audio rate as a 'buzzer' bass
// source -- the signature Spectrum sound, and the AY does not sound like
// an AY without it." This is that generator: a 5-bit (0-31) ramp counter,
// not the 4-bit ADSR shape SID needed EnvSid for -- a different enough
// mechanism (one period register, one 4-bit shape select, no attack/decay/
// sustain/release nibbles) that it earns its own type rather than trying
// to force-fit EnvSid's shape.
//
// Ground truth is ayumi.c's Envelopes[16][2] table and its
// update_envelope()/reset_segment() pair, ported into this project's
// enum-and-switch style rather than ayumi's function-pointer table (same
// information, no functional difference) -- see ay_osc.h's header for the
// licensing/vendoring note this shares.

// ---------------------------------------------------------------------------
// Envelope shape: two "segments" (0 then 1, alternating every time the ramp
// hits an end), each independently one of four behaviours. The 4-bit
// register value indexes directly into AY_ENVELOPE_SEGMENTS -- ports
// ayumi.c's Envelopes[16][2] verbatim (same 16 raw codes, same 10
// musically-distinct shapes: several raw codes share a segment pair).
// ---------------------------------------------------------------------------
enum AyEnvSegment : uint8_t { AY_ENV_SLIDE_DOWN, AY_ENV_SLIDE_UP, AY_ENV_HOLD_TOP, AY_ENV_HOLD_BOTTOM };

static const AyEnvSegment AY_ENVELOPE_SEGMENTS[16][2] = {
    { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_BOTTOM }, { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_BOTTOM },
    { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_BOTTOM }, { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_BOTTOM },
    { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_BOTTOM }, { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_BOTTOM },
    { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_BOTTOM }, { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_BOTTOM },
    { AY_ENV_SLIDE_DOWN, AY_ENV_SLIDE_DOWN },  { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_BOTTOM },
    { AY_ENV_SLIDE_DOWN, AY_ENV_SLIDE_UP },    { AY_ENV_SLIDE_DOWN, AY_ENV_HOLD_TOP },
    { AY_ENV_SLIDE_UP,   AY_ENV_SLIDE_UP },    { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_TOP },
    { AY_ENV_SLIDE_UP,   AY_ENV_SLIDE_DOWN },  { AY_ENV_SLIDE_UP,   AY_ENV_HOLD_BOTTOM },
};

// ---------------------------------------------------------------------------
// DAC tables -- re-typed directly from ayumi.c's AY_dac_table/YM_dac_table
// (Peter Sovietov, MIT, see ay_osc.h's provenance note). Unlike
// sid_tables.h's filter LUTs, these are NOT independently fitted: MIT
// permits reuse outright, so there is no licensing reason to re-derive them,
// only to attribute them, which this comment and tools/ay_ref/ayumi/LICENSE
// both do.
//
// 32 entries, not 16: index is `envelope_level` (0-31, the ramp's own unit)
// when the envelope drives a channel, or `volume*2 + 1` (landing on odd
// indices only) when the channel's own 4-bit volume register drives it
// instead -- one table serves both paths. The AY-3-8910 table's adjacent
// pairs are equal (a real 16-level DAC read at 32 resolution); the YM2149
// table's are not -- the real, documented difference between the two chips'
// analog output stages, not a rounding artifact.
// ---------------------------------------------------------------------------
static const float AY_DAC_TABLE[32] = {
    0.0f, 0.0f,
    0.00999465934234f, 0.00999465934234f,
    0.0144502937362f, 0.0144502937362f,
    0.0210574502174f, 0.0210574502174f,
    0.0307011520562f, 0.0307011520562f,
    0.0455481803616f, 0.0455481803616f,
    0.0644998855573f, 0.0644998855573f,
    0.107362478065f, 0.107362478065f,
    0.126588845655f, 0.126588845655f,
    0.20498970016f, 0.20498970016f,
    0.292210269322f, 0.292210269322f,
    0.372838941024f, 0.372838941024f,
    0.492530708782f, 0.492530708782f,
    0.635324635691f, 0.635324635691f,
    0.805584802014f, 0.805584802014f,
    1.0f, 1.0f,
};

static const float YM_DAC_TABLE[32] = {
    0.0f, 0.0f,
    0.00465400167849f, 0.00772106507973f,
    0.0109559777218f, 0.0139620050355f,
    0.0169985503929f, 0.0200198367285f,
    0.024368657969f, 0.029694056611f,
    0.0350652323186f, 0.0403906309606f,
    0.0485389486534f, 0.0583352407111f,
    0.0680552376593f, 0.0777752346075f,
    0.0925154497597f, 0.111085679408f,
    0.129747463188f, 0.148485542077f,
    0.17666895552f, 0.211551079576f,
    0.246387426566f, 0.281101701381f,
    0.333730067903f, 0.400427252613f,
    0.467383840696f, 0.53443198291f,
    0.635172045472f, 0.75800717174f,
    0.879926756695f, 1.0f,
};

enum AyModel : uint8_t { AY_MODEL_AY8910 = 0, AY_MODEL_YM2149 = 1 };

inline const float *ay_dac_table(AyModel model) {
    return model == AY_MODEL_YM2149 ? YM_DAC_TABLE : AY_DAC_TABLE;
}

// ---------------------------------------------------------------------------
// The envelope generator itself.
// ---------------------------------------------------------------------------
struct AyEnvelope {
    uint32_t period;    // 16-bit, register units, clamped >= 1
    uint32_t counter;
    uint8_t  shape;      // 4-bit, indexes AY_ENVELOPE_SEGMENTS
    uint8_t  segment;    // 0 or 1, which half of the shape is active
    int8_t   level;      // 0-31, the ramp's own unit -- also a direct index
                          // into ay_dac_table() when a channel is envelope-driven

    void init() {
        period = 1;
        counter = 0;
        shape = 0;
        segment = 0;
        level = 0;
    }

    void set_period(uint16_t reg16) {
        period = reg16 ? reg16 : 1;
    }

    // Writing a shape always restarts the envelope from its first segment --
    // ayumi_set_envelope_shape's counter/segment reset, same as a fresh
    // trigger. There is no "resume from wherever it was" case on this
    // hardware, unlike SID's gate_on legato behaviour (env_sid.h).
    void set_shape(uint8_t reg4) {
        shape = reg4 & 0xf;
        segment = 0;
        counter = 0;
        reset_segment();
    }

    inline void advance(uint32_t ticks) {
        counter += ticks;
        while (counter >= period) {
            counter -= period;
            step();
        }
    }

private:
    inline void reset_segment() {
        AyEnvSegment seg = AY_ENVELOPE_SEGMENTS[shape][segment];
        level = (seg == AY_ENV_SLIDE_DOWN || seg == AY_ENV_HOLD_TOP) ? 31 : 0;
    }

    inline void step() {
        AyEnvSegment seg = AY_ENVELOPE_SEGMENTS[shape][segment];
        switch (seg) {
            case AY_ENV_SLIDE_UP:
                level++;
                if (level > 31) { segment ^= 1; reset_segment(); }
                break;
            case AY_ENV_SLIDE_DOWN:
                level--;
                if (level < 0) { segment ^= 1; reset_segment(); }
                break;
            default:
                break;   // HOLD_TOP / HOLD_BOTTOM: level does not move
        }
    }
};

// ---------------------------------------------------------------------------
// Mixer: one channel's tone/noise gate plus its volume-or-envelope level,
// through the DAC table. t_off/n_off are *disable* flags (1 = this term
// contributes nothing to the AND, letting the other side pass unmasked) --
// ayumi_set_mixer's own naming, matching the AY-3-8910's mixer register.
// Setting both t_off and n_off is a real, documented AY quirk: the channel
// then outputs a constant level (from `(1) & (1) = 1`), not silence.
// ---------------------------------------------------------------------------
inline float ay_mix(AyModel model, uint8_t tone_out, uint8_t t_off, uint8_t noise_out, uint8_t n_off,
                     bool env_on, uint8_t volume4, int8_t env_level) {
    uint8_t gate = (uint8_t)((tone_out | t_off) & (noise_out | n_off));
    int8_t level = env_on ? env_level : (int8_t)(volume4 * 2 + 1);
    return gate ? ay_dac_table(model)[level] : 0.0f;
}
