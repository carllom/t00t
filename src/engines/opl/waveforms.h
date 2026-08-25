#pragma once

#include "opl_scale.h"
#include <cmath>
#include <cstdint>

// OPL3/4's eight selectable operator waveforms (patch.h's OplOpParams::ws),
// each a full OPL_TABLE_SIZE-entry table so op_render/op_render_first/
// op_render_fb (../fm/op.h) index them exactly like FM's single sine table --
// no runtime waveform-shaping branch in the per-sample kernel, only a
// different `table` pointer chosen at note-on.
//
// ws 0-3 (OPL2's original set) are a plausible approximation of the real
// chip's shapes (sine / half-sine / full-wave-rectified / a shortened
// quarter-cycle pulse), not a port of Yamaha's own logarithmic sine +
// antilog tables -- exact reproduction is out of scope for this pass. ws 4-6
// (OPL3/4's extension) carry the same approximation license: the chip's
// log-sine ROM is just an implementation trick for these three, tracing an
// ordinary sine or square curve. ws 7 does not: its exponential decay is the
// waveform's actual geometry, so it uses the real formula rather than a
// simpler stand-in (a linear ramp would be a qualitatively different shape).
inline int16_t opl_wave_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_half_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_abs_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_quarter_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_double_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_double_abs_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_square[OPL_TABLE_SIZE];
inline int16_t opl_wave_log_saw[OPL_TABLE_SIZE];

inline void opl_init_waveforms() {
    for (uint32_t i = 0; i < OPL_TABLE_SIZE; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / (float)OPL_TABLE_SIZE;
        float s = sinf(theta);
        float s2 = sinf(2.0f * theta);

        opl_wave_sine[i] = (int16_t)(32767.0f * s);
        opl_wave_half_sine[i] = (i < OPL_TABLE_SIZE / 2) ? (int16_t)(32767.0f * s) : 0;
        opl_wave_abs_sine[i] = (int16_t)(32767.0f * fabsf(s));
        // First quarter compressed to fill [0, pi/2) with one full positive
        // hump (sin(2*theta)), silent for the remaining three quarters.
        opl_wave_quarter_sine[i] = (i < OPL_TABLE_SIZE / 4)
            ? (int16_t)(32767.0f * s2)
            : 0;
        // First half filled with one full cycle of sin(2*theta) -- the
        // positive hump ([0, pi/2)) and negative hump ([pi/2, pi)) both fall
        // out of the doubled-frequency formula on their own -- silent second
        // half.
        opl_wave_double_sine[i] = (i < OPL_TABLE_SIZE / 2) ? (int16_t)(32767.0f * s2) : 0;
        opl_wave_double_abs_sine[i] = (i < OPL_TABLE_SIZE / 2) ? (int16_t)(32767.0f * fabsf(s2)) : 0;
        opl_wave_square[i] = (i < OPL_TABLE_SIZE / 2) ? 32767 : -32767;
        // Per-half-cycle exponential decay from full scale (~96 dB range),
        // mirrored and negated on the second half -- a hard discontinuity at
        // wraparound is part of the real shape, not a bug.
        float p = (float)i / (float)OPL_TABLE_SIZE;
        opl_wave_log_saw[i] = (i < OPL_TABLE_SIZE / 2)
            ? (int16_t)(32767.0f * exp2f(-32.0f * p))
            : (int16_t)(-32767.0f * exp2f(-32.0f * (1.0f - p)));
    }
}

// Selects one of the eight tables above by OplOpParams::ws (0-7).
inline const int16_t *opl_waveform_table(uint8_t ws) {
    switch (ws & 7) {
        case 1:  return opl_wave_half_sine;
        case 2:  return opl_wave_abs_sine;
        case 3:  return opl_wave_quarter_sine;
        case 4:  return opl_wave_double_sine;
        case 5:  return opl_wave_double_abs_sine;
        case 6:  return opl_wave_square;
        case 7:  return opl_wave_log_saw;
        default: return opl_wave_sine;
    }
}
