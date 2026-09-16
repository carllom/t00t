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
    uint8_t ws;    // 0-7, waveform select (waveforms.h)
};

// The 2-op Algorithms (OPL2) plus the 4-op Algorithms (OPL3/4's four real
// four-operator connections) share one enum: which one a patch picks also
// picks its operator count (OPL_ROUTINGS below), so nothing else on OplPatch
// needs to say "2-op" or "4-op" separately.
enum OplAlgorithm : uint8_t {
    OPL_ALGO_FM,              // 2-op: op0 -> op1 (carrier)
    OPL_ALGO_ADD,             // 2-op: op0 + op1 (both carriers)
    OPL_ALGO_4OP_CHAIN,       // 4-op: op0 -> op1 -> op2 -> op3 (carrier)
    OPL_ALGO_4OP_DUAL_FM,     // 4-op: (op0 -> op1) + (op2 -> op3)
    OPL_ALGO_4OP_ADD_CHAIN,   // 4-op: op0 + (op1 -> op2 -> op3)
    OPL_ALGO_4OP_ADD_FM_ADD,  // 4-op: op0 + (op1 -> op2) + op3
};

struct OplPatch {
    const char   *name;
    OplOpParams   op[4];      // a 2-op patch (OPL_ALGO_FM/ADD) leaves op[2]/op[3] as unused padding
    uint8_t       feedback;   // 0-7, self-modulation depth -- op0 only, 2-op or 4-op alike (op2's own register is real hardware but architecturally unwired by every 4-op connection, see module_opl.md's Feedback glossary entry)
    OplAlgorithm  algorithm;
};

// The 2 fixed OPL2 2-operator Algorithms plus the 4 fixed OPL3 4-operator
// Algorithms, each expressed directly as an FmRouting literal -- real
// hardware has only these six topologies, so there is nothing to resolve at
// note-on. `num_ops` (2 or 4): fm_voice_render_block() (../fm/op.h) loops
// only `order[0..num_ops-1]`, so operator slots past `num_ops` in every
// voice's six-wide FmOp array are never visited by the per-sample kernel at
// all -- not computed-and-discarded, simply skipped. The trailing entries in
// the arrays below are unused padding, past `num_ops`.
//
// A patch's chosen feedback amount isn't compile-time, so kernel[0]/
// fb_shift[0]/clear_bus_mask on op0 are patched into a per-voice copy of
// whichever literal below applies, at note-on (opl_voice.h) -- always op0,
// 2-op or 4-op alike.

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

// The 4 real OPL3 four-operator connections (docs/research/opl3-4op-algorithms.md,
// sourced from Nuked-OPL3's OPL3_ChannelSetupAlg). Operator pair 1 is op0/op1,
// pair 2 is op2/op3; feedback is always op0 in every one of these four --
// pair 2 (op2) has its own feedback register on real hardware, but no
// connection ever reads it, so this module doesn't model it (see OplPatch's
// `feedback` field comment above).

// Full serial chain: op0 -> op1 -> op2 -> op3 (carrier).
inline constexpr FmRouting OPL_ROUTING_4OP_CHAIN = {
    /* order          */ {0, 1, 2, 3, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, 1, 2, 3, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {1, 2, 3, FM_TARGET_OUT, 4, 5},
    /* num_ops        */ 4,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};

// Two independent 2-op FM pairs, summed: (op0 -> op1) + (op2 -> op3).
inline constexpr FmRouting OPL_ROUTING_4OP_DUAL_FM = {
    /* order          */ {0, 1, 2, 3, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, 1, FM_BUS_ZERO, 3, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {1, FM_TARGET_OUT, 3, FM_TARGET_OUT, 4, 5},
    /* num_ops        */ 4,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};

// op0 additive, plus a 3-op chain: op0 + (op1 -> op2 -> op3).
inline constexpr FmRouting OPL_ROUTING_4OP_ADD_CHAIN = {
    /* order          */ {0, 1, 2, 3, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, FM_BUS_ZERO, 2, 3, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {FM_TARGET_OUT, 2, 3, FM_TARGET_OUT, 4, 5},
    /* num_ops        */ 4,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};

// op0 additive, a 2-op FM pair, and op3 additive: op0 + (op1 -> op2) + op3.
inline constexpr FmRouting OPL_ROUTING_4OP_ADD_FM_ADD = {
    /* order          */ {0, 1, 2, 3, 0, 0},
    /* kernel         */ {FM_KERNEL_FIRST, FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_PLAIN, FM_KERNEL_FIRST, FM_KERNEL_FIRST},
    /* in_bus         */ {FM_BUS_ZERO, FM_BUS_ZERO, 2, FM_BUS_ZERO, FM_BUS_ZERO, FM_BUS_ZERO},
    /* out_bus        */ {FM_TARGET_OUT, 2, FM_TARGET_OUT, FM_TARGET_OUT, 4, 5},
    /* num_ops        */ 4,
    /* clear_bus_mask */ 0,
    /* fb_shift       */ {0, 0, 0, 0, 0, 0},
    /* valid          */ true,
};

// Indexed directly by OplAlgorithm -- order must track the enum exactly.
inline constexpr FmRouting OPL_ROUTINGS[] = {
    OPL_ROUTING_FM, OPL_ROUTING_ADD,
    OPL_ROUTING_4OP_CHAIN, OPL_ROUTING_4OP_DUAL_FM,
    OPL_ROUTING_4OP_ADD_CHAIN, OPL_ROUTING_4OP_ADD_FM_ADD,
};
static constexpr uint8_t OPL_ALGO_COUNT = 6;

// Bounds-checked lookup into OPL_ROUTINGS -- every compile-time patch
// literal in this codebase has a valid `algorithm`, but an out-of-range
// value reaching this engine (a corrupted patch, or a future bank
// converter's malformed data) would otherwise index past the array on an
// MCU with no MMU to catch it. Wraps via modulo rather than clamping to one
// fixed fallback algorithm, so every possible byte value still lands on a
// real, playable routing.
inline const FmRouting &opl_routing_for(OplAlgorithm algo) {
    return OPL_ROUTINGS[(uint8_t)algo % OPL_ALGO_COUNT];
}
