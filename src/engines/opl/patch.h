#pragma once

#include "../fm/patch.h"  // FmRouting/FM_NUM_OPS/FM_TARGET_OUT/FM_BUS_ZERO/FmKernel -- reused unchanged, not forked
#include <cstdint>

// OPL2's own patch data: chip register fields (rate/level pairs, KSL, TL in
// 0.75 dB steps, waveform select, EG-type, feedback, key-scale-rate), not an
// extension of FM's DX7-shaped FmOpParams -- OPL has no ratio/detune/
// fixed-frequency/velocity-sensitivity/key-scaling-curve concepts, and DX7
// has no waveform select or EG-type.

// Real OPL2 frequency-multiplier register table (register value 0-15 ->
// ratio against the note's own fundamental). 11 and 13 aren't typos --
// real hardware repeats 10 and 12 at those two register codes.
static constexpr float opl_mult_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

struct OplOpParams {
    uint8_t mult;  // 0-15, frequency multiplier register (opl_mult_table above)
    uint8_t ksl;   // 0-3, key scale level: extra attenuation per octave above a reference note
    uint8_t tl;    // 0-63, output level in 0.75 dB steps (0 = loudest)
    uint8_t ar;    // 0-15, attack rate (0 = operator never turns on)
    uint8_t dr;    // 0-15, decay rate (0 = never leaves the attack ceiling)
    uint8_t sl;    // 0-15, sustain level in 3 dB steps (15 is a real hardware quirk: treated as -93 dB, i.e. no sustain)
    uint8_t rr;    // 0-15, release rate (0 = never reaches silence after note-off)
    bool    egt;   // true = sustain (hold at sl until note-off); false = percussive (decay runs straight through sl to silence)
    bool    ksr;   // key scale rate: true speeds the envelope up more per octave played
    uint8_t ws;    // 0-3, waveform select (waveforms.h)
};

enum OplAlgorithm : uint8_t { OPL_ALGO_FM, OPL_ALGO_ADD };

struct OplPatch {
    const char   *name;
    OplOpParams   op[2];
    uint8_t       feedback;   // 0-7, self-modulation depth -- op0 only, real hardware's single per-channel feedback register
    OplAlgorithm  algorithm;
};

// The two fixed OPL2 2-operator algorithms, expressed directly as FmRouting
// literals -- OPL2 has only these two topologies, so there is nothing to
// resolve at note-on. `num_ops = 2`: fm_voice_render_block() (../fm/op.h)
// loops only `order[0..num_ops-1]`, so operator slots 2-5 of every voice's
// six-wide FmOp array are never visited by the per-sample kernel at all --
// not computed-and-discarded, simply skipped. The trailing entries in the
// arrays below are unused padding, past `num_ops`.
//
// A patch's chosen feedback amount isn't compile-time, so kernel[0]/
// fb_shift[0]/clear_bus_mask on op0 are patched into a per-voice copy of
// whichever literal below applies, at note-on (opl_voice.h).

// FM chain: op0 modulates op1 (bus 1), op1 is the carrier.
inline constexpr FmRouting OPL_ROUTING_FM = {
    /* order          */ {0, 1, 0, 0, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, 1, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {1, FM_TARGET_OUT, 2, 3, 4, 5},
    /* num_ops        */ 2,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};

// Additive: op0 and op1 are both carriers, summed directly into OUT.
inline constexpr FmRouting OPL_ROUTING_ADD = {
    /* order          */ {0, 1, 0, 0, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {FM_TARGET_OUT, FM_TARGET_OUT, 2, 3, 4, 5},
    /* num_ops        */ 2,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};
