#pragma once

#include <cstdint>

// ===========================================================================
// The fixed-point contract (F2, fm2.md §2/§5).
//
// ONE anchor, from which every other number here is derived:
//
//     an operator's output, in `out[]` units, is phase deviation --
//     FM_CYCLE units == one full cycle (2*pi) on whatever it modulates.
//
// That is Dexed's own contract, measured rather than assumed (tools/fm_ref;
// fm2.md §5.6): `Sin::lookup` is full-scale 2^24, its maximum operator gain
// is exactly 2.0, and 24 bits is one cycle of its phase -- so a max-level
// Dexed operator peaks at exactly TWO full cycles of deviation, and a
// *unity*-gain one at exactly one. Carriers are not a special case there and
// are not one here: a carrier's output is the same number, reinterpreted as
// audio instead of as phase. Everything else the DX7 does -- output level,
// EG level, key scaling, velocity -- is attenuation in the log domain
// beneath this single ceiling.
//
// This replaces six mutually-cancelling constants (FM_OUT_SHIFT_CARRIER,
// FM_OUT_SHIFT_MODULATOR, FM_MOD_INPUT_SHIFT, FmOpParams::level, and
// syx2patch.py's FM_CARRIER_LEVEL_REF/FM_MODULATOR_LEVEL_REF), each of which
// existed to compensate for another. fm2.md §5.1 measured what that cost:
// per-patch level errors from -12 dB to -96 dB and brightness from 0.30x to
// 6.31x on the same build, because with no shared anchor every patch landed
// somewhere different.
//
// Do not "tune" anything in this block. If a patch is too loud or too dim,
// the answer is in its output level, its EG, or its key scaling -- exactly as
// it would be on real hardware. That discipline is the entire point.
// ===========================================================================

// One full cycle of phase deviation, in bus units. 2^26 leaves 5 bits of
// headroom over the 2^27 maximum a single operator can emit (see
// FM_GAIN_MAX), which covers both fan-in (several modulators summing onto one
// bus) and several carriers summing into the output bus, without ever
// approaching int32 overflow -- the failure mode fm2.md §1.1(a) found in the
// old scaling, where a max-level modulator reached 2^33.4 and wrapped the
// phase accumulator several times per sample.
static constexpr uint32_t FM_CYCLE_BITS = 26;
static constexpr int32_t  FM_CYCLE = 1 << FM_CYCLE_BITS;

// Bus units -> the 32-bit phase accumulator (2^32 == one cycle). Unsigned
// wraparound past a full cycle is correct by construction, not a lucky
// accident: that is exactly what a phase accumulator is for.
static constexpr uint32_t FM_MOD_SHIFT = 32 - FM_CYCLE_BITS;

// The gain at log2 offset 0 -- i.e. output level 99, EG at level 99, no key
// scaling, no velocity attenuation. Derived, not chosen: fm_mul_gain() does
// (gain * sample) >> 16 with the sine table full-scale at 2^15, and the
// contract says that product must be 2 * FM_CYCLE at maximum level (Dexed's
// measured 2.0 gain ceiling), so FM_GAIN_MAX = 2^(1 + 26 + 16 - 15) = 2^28.
// env_dx.h's eg_to_gain() returns exactly this at the envelope's ceiling.
static constexpr int32_t FM_GAIN_MAX = 1 << 28;

// Voice output bus -> int16 audio, applied once in fm_render_voice(). A
// single max-level carrier lands at 2^14, i.e. half of int16 full scale, so
// a two-carrier patch at full tilt reaches 0 dBFS and busier algorithms rely
// on the mixer's existing saturation -- the same bargain the hardware makes.
// This is the one master headroom choice in the engine; it is a property of
// how loud a *voice* should be, not of any operator, and nothing else in the
// signal path is allowed to have an opinion about level.
static constexpr int32_t FM_VOICE_OUT_SHIFT = FM_CYCLE_BITS - 12;
