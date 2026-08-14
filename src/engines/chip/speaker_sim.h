#pragma once

#include "res2p.h"
#include <cmath>

// Speaker simulation output stage -- chip.md §1 P5, §10 (HP cone rolloff ->
// resonant "boxy" peak -> LP -> soft clip). Downstream of the FX insert
// (delay/reverb) and upstream of the final int16 __ssat -- §10's own "you
// want delay -> speaker, or reverb -> speaker", and open question 4
// (resolved): the stage's own soft clip is the cone-breakup *character*,
// a tonal choice, not the final safety clamp, so it runs before that clamp
// rather than after it.
//
// Float, not the chip primitives' fixed-point: this sits downstream of the
// insert (delay/reverb), which are already float (fx/reverb.h's Freeverb),
// so there is no fixed-point contract to match here the way sid_voice.h's
// DAC chain has to match reSID exactly. Divides in here are real hardware
// vdiv.f32 (this target's -mfloat-abi=softfp still has the FPU), not the
// software 64-bit divides sid_filter.h's saturate() had -- no reciprocal
// trick needed.

enum SpeakerPreset : uint8_t {
    SPEAKER_1702,     // Commodore 1702: a real (decent, by home-computer-
                       // monitor standards) RGB/composite monitor speaker --
                       // the *reference* SID sound most people actually heard
                       // it through. Cleanest of the five: mild peak, open top.
    SPEAKER_TV,        // A portable TV set next to the machine. Smaller cone
                       // than the 1702, boxier mid hump, duller top.
    SPEAKER_GAMEBOY,   // Tiny mono speaker, the whole point of the module's
                       // "period-correct playback ... through a TV or monitor
                       // speaker" idea pushed to its cheapest extreme: narrow,
                       // pronounced boxy peak, rolled-off top, hardest clip.
    SPEAKER_ARCADE,    // Cabinet speaker: more cone area than any of the
                       // above, so more real bass extension (§10's HP range's
                       // low end) and a broader, milder hump -- but classic
                       // cabinet amps were routinely driven hard, hence a
                       // stronger clip than the 1702 despite the bigger cone.
    SPEAKER_BYPASS,    // Flat -- CHIP_STRICT-style escape hatch for A/B
                       // comparison against the unsimulated signal.
    SPEAKER_PRESET_COUNT,
};

static const char *const SPEAKER_PRESET_NAMES[SPEAKER_PRESET_COUNT] = {
    "1702 monitor", "portable TV", "Game Boy", "arcade cab", "bypass",
};

struct SpeakerPresetDef {
    float hp_hz;     // one-pole HP corner, §10's ~150-300 Hz cone-rolloff range
    float peak_hz;   // res2p center, §10's ~400-800 Hz "boxy hump"
    float peak_bw;   // res2p -3dB bandwidth -- narrower = more pronounced hump
    float lp_hz;      // one-pole LP corner, §10's ~5-9 kHz range
    float drive;      // pre-gain into the fixed cubic soft-clip shape below --
                       // one clip curve, per-preset breakup amount from drive
                       // alone, cheaper than a second curve per preset
};

// Numbers below are a first, doc-range-respecting pass, not a hardware-
// measured or by-ear-tuned set -- same status P3's vibrato constants had
// before Carl's by-ear pass (chip.md §14d.5): "a better first guess, not a
// calibrated one." Needs a real listen before it's trusted.
static const SpeakerPresetDef SPEAKER_PRESETS[SPEAKER_PRESET_COUNT] = {
    // hp_hz  peak_hz  peak_bw  lp_hz    drive
    {  150.0f,  500.0f,  250.0f, 9000.0f, 1.0f },  // SPEAKER_1702 -- open, mild
    {  300.0f,  700.0f,  200.0f, 5000.0f, 1.4f },  // SPEAKER_TV -- boxy, duller
    {  300.0f,  800.0f,  150.0f, 5000.0f, 2.0f },  // SPEAKER_GAMEBOY -- thin, crunchy
    {  150.0f,  400.0f,  300.0f, 7000.0f, 1.7f },  // SPEAKER_ARCADE -- broad hump, driven hard
    {   20.0f, 1000.0f, 4000.0f, 20000.0f, 1.0f }, // SPEAKER_BYPASS -- unused, see tick():
                                                     // a true short-circuit, not run through
                                                     // this chain with wide-open corners
};

struct SidSpeakerStage {
    float hp_lp_state = 0.0f;   // HP built as x - lowpass(x), per §10's "one-pole"
    float lp_state = 0.0f;
    float hp_a = 0.0f, lp_a = 0.0f;
    float drive = 1.0f;
    Res2p peak;
    float sample_rate = 44100.0f;
    uint8_t current_preset = 0xFF;   // 0xFF: force apply_preset() on first use

    void init(float fs) {
        hp_lp_state = 0.0f;
        lp_state = 0.0f;
        peak = Res2p{};
        sample_rate = fs;
        current_preset = 0xFF;
        apply_preset(SPEAKER_1702);
    }

    // res2p.h's own stability rule: recompute coefficients via res2p_set(),
    // never interpolate a1/a2/b0 directly. Cheap to guard on "did the
    // preset actually change" since this would otherwise run every buffer.
    void apply_preset(uint8_t id) {
        if (id == current_preset || id >= SPEAKER_PRESET_COUNT) return;
        current_preset = id;
        const SpeakerPresetDef &p = SPEAKER_PRESETS[id];
        hp_a = 1.0f - expf(-2.0f * (float)M_PI * p.hp_hz / sample_rate);
        lp_a = 1.0f - expf(-2.0f * (float)M_PI * p.lp_hz / sample_rate);
        res2p_set(peak, p.peak_hz, p.peak_bw, sample_rate);
        drive = p.drive;
    }

    inline float tick(float x) {
        // SPEAKER_BYPASS: a real short-circuit, not "corners tuned wide
        // enough to sound flat" -- Carl heard the difference (a "tiny bit
        // dull" against the pre-P5 build). Root cause: res2p's own LUT
        // clamps peak_bw to RES2P_RADIUS_X_MAX (res2p.h) rather than
        // extrapolating, so the intended ~4 kHz bandwidth silently became
        // ~1.1 kHz at this sample rate -- a real, if broad, resonant bump,
        // not the negligible one the SPEAKER_PRESETS row aimed for. A
        // resonant 2-pole filter genuinely cannot be tuned into a flat
        // response by widening bw past what the pole radius supports; only
        // an actual passthrough is guaranteed transparent.
        if (current_preset == SPEAKER_BYPASS) return x;

        hp_lp_state += (x - hp_lp_state) * hp_a;
        float hp_out = x - hp_lp_state;

        float peaked = res2p_tick(peak, hp_out);

        lp_state += (peaked - lp_state) * lp_a;
        float y = lp_state * drive;

        // Soft clip / cone breakup: same cubic shape as sid_filter_saturate,
        // scaled to this stage's float working range instead of the chip
        // primitives' int32 one. Per-preset character comes from `drive`
        // (above) pushing more or less signal into this fixed curve, not
        // from varying the curve itself.
        constexpr float lim = 24000.0f;
        if (y >= lim)  return  lim * (2.0f / 3.0f);
        if (y <= -lim) return -lim * (2.0f / 3.0f);
        float yn = y / lim;
        return y - (y * yn * yn) / 3.0f;
    }
};
