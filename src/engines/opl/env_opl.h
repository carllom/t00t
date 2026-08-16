#pragma once

#include "../fm/env_dx.h"  // eg_to_gain()/EG_LEVEL_MAX/EG_LEVEL_ONE_OCTAVE -- reused unchanged
#include "patch.h"
#include <cmath>
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
// one known curve-shape gap; the rate each stage ramps at is ported from
// Nuked-OPL3 below (see opl_ctl_diff.py).
//
// OPL2 hardware has no velocity input at all -- MIDI velocity is folded onto
// the same output-level attenuation TL/KSL use below, a t00t-side addition
// for MIDI playability rather than something the real chip does.

// Real hardware's SL=15 quirk: decoded as far past any of the other 15
// linear 3 dB steps, deep enough that reaching it is indistinguishable from
// true silence.
static constexpr float OPL_SL15_DB = 93.0f;

static constexpr float OPL_DB_PER_OCTAVE = 6.0206f;  // 20*log10(2)

inline int32_t opl_db_to_level(float db) {
    int32_t level = (int32_t)((db / OPL_DB_PER_OCTAVE) * (float)EG_LEVEL_ONE_OCTAVE);
    if (level < 0) level = 0;
    if (level > EG_LEVEL_MAX) level = EG_LEVEL_MAX;
    return level;
}

// Real hardware's own envelope unit: one step of the chip's internal
// attenuation register, which TL, KSL, and the envelope generator below all
// share (Nuked-OPL3's eg_out = eg_rout + (tl<<2) + (ksl>>shift) + tremolo).
// Composing in this domain and converting to the shared Q24-octave domain
// only once is what keeps TL/KSL exact matches to Nuked-OPL3's own numbers.
static constexpr float OPL_UNIT_DB = 0.1875f;

// Real hardware's key-scale-level ROM, an exact copy of Nuked-OPL3's
// kslrom[]/kslshift[] -- indexed by the top 4 bits of the note's own
// f-number (ROM) and by the KSL register 0-3 (shift).
static constexpr uint8_t OPL_KSL_ROM[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};
static constexpr uint8_t OPL_KSL_SHIFT[4] = { 8, 1, 2, 0 };

// Real chip PG clock divisor (chip clock / 72), used below to re-derive the
// block/f-number a real OPL2 would have been programmed with for a given
// note -- t00t itself never encodes pitch this way (its phase increment
// comes from a plain float frequency), but KSL and key-scale-rate both read
// this encoding off the chip's own registers, not the note directly.
static constexpr float OPL_CLOCK_OVER_72 = 49716.0f;

inline void opl_note_to_fnum_block(uint8_t midinote, uint16_t &fnum, uint8_t &block) {
    float freq = 440.0f * exp2f(((float)midinote - 69.0f) / 12.0f);
    for (uint8_t b = 0; b <= 7; b++) {
        float f = freq * (float)(1u << (20 - b)) / OPL_CLOCK_OVER_72;
        if (f <= 1023.0f) { block = b; fnum = (uint16_t)(f + 0.5f); return; }
    }
    block = 7;
    fnum = 1023;
}

// Real hardware's own KSL formula (Nuked-OPL3's OPL3_EnvelopeUpdateKSL): a
// ROM lookup on the note's f-number, scaled down per octave below block 7,
// floored at 0.
inline float opl_ksl_db(uint8_t ksl_reg, uint16_t fnum, uint8_t block) {
    int32_t ksl = ((int32_t)OPL_KSL_ROM[fnum >> 6] << 2) - ((8 - (int32_t)block) << 5);
    if (ksl < 0) ksl = 0;
    return (float)(ksl >> OPL_KSL_SHIFT[ksl_reg & 3]) * OPL_UNIT_DB;
}

// Real hardware's key-scale value: block and the top bit of f-number, packed
// the same way the chip's own B0 register write derives it.
inline uint8_t opl_ksv(uint16_t fnum, uint8_t block) {
    return (uint8_t)((block << 1) | ((fnum >> 9) & 1));
}

// The combined 0..63 rate real hardware's envelope generator actually runs
// at: the 4-bit register rate plus a key-scale contribution (ksv, halved
// twice when KSR is off) -- Nuked-OPL3's `rate`/`ks` in OPL3_EnvelopeCalc.
// Register rate 0 stays "never" regardless of ksv, matching real hardware
// (ksv alone never restarts a stopped stage).
inline uint8_t opl_combined_rate(uint8_t reg_rate, bool ksr, uint8_t ksv) {
    if (reg_rate == 0) return 0;
    uint8_t ks = ksr ? ksv : (uint8_t)(ksv >> 2);
    uint16_t rate = (uint16_t)ks + ((uint16_t)reg_rate << 2);
    return (uint8_t)(rate > 63 ? 63 : rate);
}

// Real hardware doubles its envelope speed every 4 steps of the combined
// rate above (Nuked-OPL3's rate_hi/eg_add bit-trick clock divider) --
// confirmed by measuring nuked_dump's own eg trajectories across the full
// rate range. `ref_time_s` is the real, measured time (seconds) for the
// stage to cross the full 511-unit register range at combined rate 32.
static constexpr uint8_t OPL_EG_REF_RATE = 32;
// Decay/release (DR/RR register 8, no key scale) measured against
// Nuked-OPL3.
static constexpr float OPL_EG_DECAY_REF_TIME_S = 0.3287912f;
// Attack (AR register 8, no key scale) measured against Nuked-OPL3. Real
// attack is a curved, fast-then-slower approach to the ceiling (the
// documented shape gap above); this reference time at least lands the
// linear ramp's total duration on the real hardware number instead of a
// guess.
static constexpr float OPL_EG_ATTACK_REF_TIME_S = 0.02229f;

// Q24-octaves-per-sample magnitude for a given combined rate. Rate 0 (or a
// rate whose register value was 0, folded in by opl_combined_rate above)
// returns 0 -- the stage never reaches its target.
inline int32_t opl_rate_inc(uint8_t combined_rate, float ref_time_s) {
    if (combined_rate == 0) return 0;
    float units_per_s = (511.0f / ref_time_s) *
        exp2f(((float)combined_rate - (float)OPL_EG_REF_RATE) / 4.0f);
    float octaves_per_s = units_per_s * (OPL_UNIT_DB / OPL_DB_PER_OCTAVE);
    return (int32_t)(octaves_per_s * (float)EG_LEVEL_ONE_OCTAVE / (float)SAMPLE_RATE);
}

struct EnvOpl {
    uint8_t ar, dr, sl_reg, rr;  // raw 4-bit register values
    bool    egt;
    bool    ksr;
    uint8_t ksv;  // real hardware's key-scale value, resolved at note-on from the note's block/f-number

    int32_t ceiling;       // Q24 octaves -- attack's target, resolved at note-on from TL/KSL/velocity
    int32_t sustain_level;  // Q24 octaves -- decay's target (stage 1), sustain or percussive alike

    int32_t level;       // Q24 octaves, current
    int32_t targetlevel;  // Q24 octaves, current stage's target
    int32_t inc;          // Q24-octaves-per-sample magnitude for the current stage
    bool    rising;
    uint8_t ix;    // 0=attack, 1=decay, 2=sustain (hold if egt, else continues to silence at the release rate), 3=release, 4=idle
    bool    down;  // note held

    bool active() const { return ix < 4; }
};

inline uint8_t env_opl_next_ix(const EnvOpl &eg);

// Recomputes the stage target/rate/direction. Called on init, on note-off,
// and whenever a stage completes.
inline void env_opl_advance(EnvOpl &eg, int newix) {
    eg.ix = (uint8_t)newix;
    if (eg.ix >= 4) return;

    uint8_t rate;
    float ref_time_s = OPL_EG_DECAY_REF_TIME_S;
    switch (eg.ix) {
        case 0: eg.targetlevel = eg.ceiling;      rate = eg.ar; ref_time_s = OPL_EG_ATTACK_REF_TIME_S; break;
        // Decay always targets the sustain level, sustain mode or
        // percussive alike -- real hardware's own decay stage has no idea
        // yet whether stage 2 below will hold there or keep going.
        case 1: eg.targetlevel = eg.sustain_level; rate = eg.dr; break;
        // Sustain mode holds here (rate 0, forced by the reg_rate==0 check
        // inside opl_combined_rate below). Percussive mode does not hold at
        // all: real hardware keeps moving past the sustain level toward
        // silence, at the RELEASE rate, not decay's -- Nuked-OPL3's
        // envelope_gen_num_sustain case falls through to reg_rr whenever
        // reg_type (EGT) is 0.
        case 2: if (eg.egt) { eg.targetlevel = eg.sustain_level; rate = 0; }
                else        { eg.targetlevel = 0;                rate = eg.rr; }
                break;
        default: eg.targetlevel = 0;               rate = eg.rr; break;  // release
    }
    eg.rising = eg.targetlevel > eg.level;

    uint8_t combined = opl_combined_rate(rate, eg.ksr, eg.ksv);
    // Real hardware's attack has one more quirk beyond the doubling law
    // above: at the very top of its rate range it jumps to the ceiling in a
    // single step rather than ramping (Nuked-OPL3's "instant attack" case,
    // rate_hi==15 on a fresh attack) -- reproduced directly here rather than
    // folded into the smooth curve, since it is a genuine discontinuity, not
    // a fast-but-continuous rate.
    if (eg.ix == 0 && combined >= 60) {
        eg.level = eg.targetlevel;
        env_opl_advance(eg, env_opl_next_ix(eg));
        return;
    }
    eg.inc = opl_rate_inc(combined, ref_time_s);
}

// The stage index to advance into once the current one completes.
// Percussive mode's stage 2 (see above) already reached silence under its
// own power once it gets here -- there is no release left to run when
// note-off eventually arrives, so it goes straight to idle instead of a
// release stage that would just restate the same silent target.
inline uint8_t env_opl_next_ix(const EnvOpl &eg) {
    if (eg.ix == 2 && !eg.egt) return 4;
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
    eg.egt = p.egt; eg.ksr = p.ksr;

    uint16_t fnum; uint8_t block;
    opl_note_to_fnum_block(midinote, fnum, block);
    eg.ksv = opl_ksv(fnum, block);

    int velocity = ((int)amplitude * 127 + 16383) / 32767;
    float tl_db = (float)p.tl * 0.75f;
    float ksl_db = opl_ksl_db(p.ksl, fnum, block);
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

    // Covers both sustain mode's real hold (stage 2, rate forced to 0 by
    // opl_combined_rate) and a register rate of 0 on any other stage (real
    // hardware's own "never" case).
    if (eg.inc == 0) return eg.level;

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
