#pragma once

#include "../fm/env_dx.h"  // eg_to_gain()/EG_LEVEL_MAX/EG_LEVEL_ONE_OCTAVE -- reused unchanged
#include "patch.h"
#include <cstdint>

// EnvOpl -- OPL2's 4-stage (attack/decay/sustain-hold/release) envelope.
// Shares EnvDX's own level domain (Q24 octaves, 0..EG_LEVEL_MAX) so the
// reused eg_to_gain() (env_dx.h) converts its output straight into a
// kernel-ready gain with no new conversion code anywhere.
//
// Every stage here is a straight linear ramp in that log domain. Real OPL2's
// attack is a curved (fast-then-slower) shape, not linear -- a deliberate
// simplification for a first working implementation, not a claim of chip
// accuracy. Decay/release ramping linearly in the log domain is a real,
// intentional envelope shape rather than a simplification, so attack is the
// one known curve-shape gap.
//
// OPL2 hardware has no velocity input at all -- MIDI velocity is folded onto
// the same output-level attenuation TL/KSL use below, a t00t-side addition
// for MIDI playability rather than something the real chip does.

// Real hardware's SL=15 quirk: decoded as far past any of the other 15
// linear 3 dB steps, deep enough that reaching it is indistinguishable from
// true silence.
static constexpr float OPL_SL15_DB = 93.0f;

// dB per octave of attenuation for each of the 4 KSL settings.
static constexpr float OPL_KSL_STEP_DB[4] = {0.0f, 1.5f, 3.0f, 6.0f};

// MIDI note above which KSL attenuation starts to apply. Not derived from
// the real chip's block/F-number encoding -- a plausible flat reference
// point for this pass.
static constexpr uint8_t OPL_KSL_BREAKPOINT_NOTE = 48;

static constexpr float OPL_DB_PER_OCTAVE = 6.0206f;  // 20*log10(2)

inline int32_t opl_db_to_level(float db) {
    int32_t level = (int32_t)((db / OPL_DB_PER_OCTAVE) * (float)EG_LEVEL_ONE_OCTAVE);
    if (level < 0) level = 0;
    if (level > EG_LEVEL_MAX) level = EG_LEVEL_MAX;
    return level;
}

// Time (seconds) to cross the full 15-octave level range at each of the 16
// rate register values. Geometric, roughly halving per step -- a plausible
// spread from slow (attack/decay/release all audibly gradual) to
// near-instant, not a port of the real chip's own rate table. Index 0 is
// unused by the time lookup (rate 0 always means "never move," handled as a
// special case) but kept for a flat 16-entry array.
static constexpr float OPL_RATE_TIME_S[16] = {
    0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f,
    0.03125f, 0.015625f, 0.0078125f, 0.00390625f, 0.001953125f,
    0.0009765625f, 0.00048828125f, 0.000244140625f,
};

// Key scale rate: higher notes get a faster effective rate, matching the
// general "higher notes decay quicker" behavior real OPL2's KSR bit
// describes. Rate 0 stays 0 regardless (it means "never," not "very slow").
inline uint8_t opl_effective_rate(uint8_t rate, bool ksr, uint8_t midinote) {
    if (rate == 0) return 0;
    int boost = ksr ? (midinote >> 4) : (midinote >> 5);
    int r = (int)rate + boost;
    return (uint8_t)(r > 15 ? 15 : r);
}

// Q24-octaves-per-sample magnitude for a given (already key-scaled) rate.
// Rate 0 returns 0 -- the stage never reaches its target.
inline int32_t opl_rate_inc(uint8_t eff_rate) {
    if (eff_rate == 0) return 0;
    float t = OPL_RATE_TIME_S[eff_rate];
    return (int32_t)((float)EG_LEVEL_MAX / (t * (float)SAMPLE_RATE));
}

struct EnvOpl {
    uint8_t ar, dr, sl_reg, rr;  // raw 4-bit register values
    bool    egt;
    bool    ksr;
    uint8_t midinote;

    int32_t ceiling;       // Q24 octaves -- attack's target, resolved at note-on from TL/KSL/velocity
    int32_t sustain_level;  // Q24 octaves -- decay's target when egt == sustain

    int32_t level;       // Q24 octaves, current
    int32_t targetlevel;  // Q24 octaves, current stage's target
    int32_t inc;          // Q24-octaves-per-sample magnitude for the current stage
    bool    rising;
    uint8_t ix;    // 0=attack, 1=decay, 2=sustain-hold, 3=release, 4=idle
    bool    down;  // note held

    bool active() const { return ix < 4; }
};

// Recomputes the stage target/rate/direction. Called on init, on note-off,
// and whenever a stage completes.
inline void env_opl_advance(EnvOpl &eg, int newix) {
    eg.ix = (uint8_t)newix;
    if (eg.ix >= 4) return;

    uint8_t rate;
    switch (eg.ix) {
        case 0:  eg.targetlevel = eg.ceiling;        rate = eg.ar; break;
        // Percussive mode has no separate hold stage: decay runs straight
        // through the sustain level to silence, so its target is 0 rather
        // than sustain_level.
        case 1:  eg.targetlevel = eg.egt ? eg.sustain_level : 0; rate = eg.dr; break;
        case 2:  eg.targetlevel = eg.sustain_level;   rate = 0;    break;  // hold -- inc forced to 0 below
        default: eg.targetlevel = 0;                  rate = eg.rr; break;  // release
    }
    eg.rising = eg.targetlevel > eg.level;
    eg.inc = (eg.ix == 2) ? 0 : opl_rate_inc(opl_effective_rate(rate, eg.ksr, eg.midinote));
}

// The stage index to advance into once the current one completes. Decay
// (stage 1) skips straight to idle in percussive mode -- it already ramped
// to silence itself, so there is no hold stage left to enter, and leaving a
// finished percussive envelope sitting at stage 2 would keep it (and the
// voice it belongs to) permanently reporting itself active.
inline uint8_t env_opl_next_ix(const EnvOpl &eg) {
    if (eg.ix == 1 && !eg.egt) return 4;
    return (uint8_t)(eg.ix + 1);
}

// Note-on composition: output level (TL) + key scale level (KSL) + MIDI
// velocity all attenuate the same ceiling the attack stage rises to, same
// spot DX7's outlevel composition folds output level/key scaling/velocity
// into one number (env_dx.h's dx7_note_outlevel()) -- just OPL-native units
// converted to dB, then to this shared Q24-octave domain, instead of DX7
// microsteps.
inline void env_opl_init(EnvOpl &eg, const OplOpParams &p, uint8_t midinote, int16_t amplitude) {
    eg.ar = p.ar; eg.dr = p.dr; eg.sl_reg = p.sl; eg.rr = p.rr;
    eg.egt = p.egt; eg.ksr = p.ksr; eg.midinote = midinote;

    int velocity = ((int)amplitude * 127 + 16383) / 32767;
    float tl_db = (float)p.tl * 0.75f;
    float octaves_above = midinote > OPL_KSL_BREAKPOINT_NOTE
        ? (float)(midinote - OPL_KSL_BREAKPOINT_NOTE) / 12.0f : 0.0f;
    float ksl_db = OPL_KSL_STEP_DB[p.ksl & 3] * octaves_above;
    float vel_db = (float)(127 - velocity) * 0.375f;

    eg.ceiling = EG_LEVEL_MAX - opl_db_to_level(tl_db + ksl_db + vel_db);
    float sl_db = (p.sl >= 15) ? OPL_SL15_DB : (float)p.sl * 3.0f;
    eg.sustain_level = eg.ceiling - opl_db_to_level(sl_db);
    if (eg.sustain_level < 0) eg.sustain_level = 0;

    eg.level = 0;
    eg.down = true;
    env_opl_advance(eg, 0);
}

// Jump to the release stage from wherever the envelope currently is. `down`
// guards against re-entering release on a second note-off: once it's false,
// this is a no-op regardless of how many more times it's called.
inline void env_opl_release(EnvOpl &eg) {
    if (eg.down) {
        eg.down = false;
        env_opl_advance(eg, 3);
    }
}

// Advances `n` samples, returns the level at the end of the block.
inline int32_t env_opl_step_block(EnvOpl &eg, uint32_t n) {
    if (eg.ix >= 4) return eg.level;
    if (eg.ix == 2) return eg.level;  // sustain hold: level does not move

    if (eg.inc == 0) return eg.level;  // rate 0: this stage never completes on its own

    int32_t delta = eg.inc * (int32_t)n;
    if (eg.rising) {
        eg.level += delta;
        if (eg.level >= eg.targetlevel) {
            eg.level = eg.targetlevel;
            env_opl_advance(eg, env_opl_next_ix(eg));
        }
    } else {
        eg.level -= delta;
        if (eg.level <= eg.targetlevel) {
            eg.level = eg.targetlevel;
            env_opl_advance(eg, env_opl_next_ix(eg));
        }
    }
    return eg.level;
}
