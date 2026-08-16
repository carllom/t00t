#pragma once

// OPL2's waveform table width. The per-sample kernels (op_render/
// op_render_first/op_render_fb, ../fm/op.h) are templated on this, so a
// 1024-entry table costs exactly as much per sample as FM's 4096-entry one --
// the phase shift is a compile-time constant either way.
//
// There is deliberately no OPL-specific gain-domain anchor here (no
// OPL_CYCLE/OPL_GAIN_MAX): the reused kernels read fm_scale.h's FM_CYCLE/
// FM_MOD_SHIFT/FM_GAIN_MAX as plain global constants, not template
// parameters, so this engine inherits that bus-unit and gain-ceiling
// convention verbatim rather than choosing its own.
static constexpr uint32_t OPL_TABLE_BITS = 10;
static constexpr uint32_t OPL_TABLE_SIZE = 1u << OPL_TABLE_BITS;  // 1024
