#pragma once

#include "opl_scale.h"
#include <cmath>
#include <cstdint>

// OPL2's four selectable operator waveforms (patch.h's OplOpParams::ws),
// each a full OPL_TABLE_SIZE-entry table so op_render/op_render_first/
// op_render_fb (../fm/op.h) index them exactly like FM's single sine table --
// no runtime waveform-shaping branch in the per-sample kernel, only a
// different `table` pointer chosen at note-on.
//
// These are a plausible approximation of the real chip's four shapes
// (sine / half-sine / full-wave-rectified / a shortened quarter-cycle
// pulse), not a port of Yamaha's own logarithmic sine + antilog tables --
// exact reproduction is out of scope for this pass.
inline int16_t opl_wave_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_half_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_abs_sine[OPL_TABLE_SIZE];
inline int16_t opl_wave_quarter_sine[OPL_TABLE_SIZE];

inline void opl_init_waveforms() {
    for (uint32_t i = 0; i < OPL_TABLE_SIZE; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / (float)OPL_TABLE_SIZE;
        float s = sinf(theta);

        opl_wave_sine[i] = (int16_t)(32767.0f * s);
        opl_wave_half_sine[i] = (i < OPL_TABLE_SIZE / 2) ? (int16_t)(32767.0f * s) : 0;
        opl_wave_abs_sine[i] = (int16_t)(32767.0f * fabsf(s));
        // First quarter compressed to fill [0, pi/2) with one full positive
        // hump (sin(2*theta)), silent for the remaining three quarters.
        opl_wave_quarter_sine[i] = (i < OPL_TABLE_SIZE / 4)
            ? (int16_t)(32767.0f * sinf(2.0f * theta))
            : 0;
    }
}

// Selects one of the four tables above by OplOpParams::ws (0-3).
inline const int16_t *opl_waveform_table(uint8_t ws) {
    switch (ws & 3) {
        case 1:  return opl_wave_half_sine;
        case 2:  return opl_wave_abs_sine;
        case 3:  return opl_wave_quarter_sine;
        default: return opl_wave_sine;
    }
}
