#pragma once

#include <cstdint>

// FM patch data + note-on-time routing compiler (#44, fm.md §4/§5.6/§7):
// the runtime FmOpParams/FmPatch shape the eventual tools/syx2patch.py
// converter (P3) will emit, plus the DAG-routing resolver that turns one of
// those patches into per-operator order/bus/kernel decisions -- fm.md §4.1's
// central claim, "an operator's routing IS its in/out bus pointers plus its
// position in the processing order, both resolved once at note-on," lives
// here. No pico-sdk dependency (plain cstdint), so this header is shared by
// both the device engine (audio_engine.cpp/midi_controller.cpp) and the host
// render/test harness (tools/host_render/render_fm.cpp), matching the
// render.h/sine_tab.h/rig.h convention already established for this engine.

static constexpr uint8_t FM_NUM_OPS = 6;

// Bus id space (fm.md §4.3): 0-5 are the six operator-indexed modulation
// buses (bus b is "the bus operator b reads as its own modulation input"),
// FM_TARGET_OUT is the shared voice output bus that carriers sum into, and
// FM_BUS_ZERO is a read-only all-zero source for operators nothing
// modulates (fm.md §5.2: "pure carriers pointing at a zero bus"). The same
// numeric value doubles as FmOpParams::mod_target's "this op is a carrier"
// sentinel and as the routing compiler's output-bus id -- a carrier's
// output IS the thing being routed to bus FM_TARGET_OUT, so one constant
// serves both roles without a separate mapping step.
static constexpr uint8_t FM_TARGET_OUT = FM_NUM_OPS;      // 6
static constexpr uint8_t FM_BUS_ZERO   = FM_NUM_OPS + 1;  // 7

// One operator's patch data -- the shape tools/syx2patch.py (P3) will emit
// per DX7 operator (fm.md §7's table): ratio/detune/fixed-frequency mode
// resolve to the Q32 increment (§5.6), `level` is the operator's reference
// gain -- what op_render's `gain` is at 100% output level, 100% EG level,
// and velocity-sensitivity-neutral (op.h's fm_mul_gain/FM_OUT_SHIFT scale,
// unity ≈ 1<<22) -- and `mod_target`/`feedback` together are the entire
// routing input: which operator (or FM_TARGET_OUT) this op's output feeds,
// and whether its own last two outputs additionally self-modulate its
// phase. `output_level`/`vel_sensitivity`/`eg_rate`/`eg_level` (#45,
// env_dx.h) are the note-on/block-rate-resolved pieces that turn `level`
// into the actual, time-varying `gain` op_render sees -- see env_dx.h for
// how they combine.
struct FmOpParams {
    float   ratio;         // coarse.fine frequency ratio against the note (ignored if fixed_freq)
    float   fixed_hz;      // absolute frequency in Hz, used only when fixed_freq is true
    bool    fixed_freq;
    float   detune_cents;  // fine detune in cents, applied on top of ratio (0 for fixed_freq)
    int32_t level;         // reference gain (env_dx.h's "0 dB" point) -- op.h's fm_mul_gain/FM_OUT_SHIFT scale
    uint8_t mod_target;    // 0..FM_NUM_OPS-1 (another operator), or FM_TARGET_OUT (carrier)
    bool    feedback;      // self-modulation: op_render_fb's 2-sample average (fm.md §5.2)
    uint8_t output_level;    // DX7 TL, 0-99, through env_dx.h's DX7_LEVEL_TO_LOG2 (#45)
    uint8_t vel_sensitivity; // 0-7, env_dx.h's eg_vel_sensitivity_log2 (#45)
    uint8_t eg_rate[4];      // R1-R4, 0-99 (#45)
    uint8_t eg_level[4];     // L1-L4, 0-99 (#45) -- L4 = 0 lets this operator actually reach silence on release
};

struct FmPatch {
    const char *name;
    FmOpParams  op[FM_NUM_OPS];
};

// Note-on-time routing decisions (fm.md §5.6): everything the per-sample
// kernel needs, with nothing in it that depends on note/velocity/bend --
// only on the patch. `order` is the topological processing order;
// `in_bus`/`out_bus` are per-operator bus ids (0-5, FM_TARGET_OUT, or
// FM_BUS_ZERO for in_bus only); `kernel` selects one of op.h's three
// variants; `clear_bus_mask` (bit b = bus b, b in 0..FM_TARGET_OUT) flags
// the rare bus whose only writer is a feedback operator -- op_render_fb
// always accumulates (fm.md §5.2), so if that's the *first* write to a bus
// it must be pre-zeroed rather than relying on the usual "no bus ever needs
// clearing" first-writer optimization (§4.3).
enum FmKernel : uint8_t { FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_FEEDBACK };

struct FmRouting {
    uint8_t order[FM_NUM_OPS];
    uint8_t kernel[FM_NUM_OPS];
    uint8_t in_bus[FM_NUM_OPS];
    uint8_t out_bus[FM_NUM_OPS];
    uint8_t clear_bus_mask;
    bool    valid;
};

// Resolves `patch` into `r`. Returns false (r.valid = false) if the patch's
// mod_target graph contains a cycle spanning two or more operators -- the
// one routing shape block-inner rendering can't evaluate without a
// block-length delay (fm.md §4.2). Self-modulation (the `feedback` flag) is
// accepted unconditionally: it never enters this graph at all, because it's
// satisfied entirely inside op_render_fb's own per-sample fb1/fb2 history,
// not by bus-write ordering -- so there is nothing for a cycle check to
// reject.
inline bool fm_resolve_routing(const FmPatch &patch, FmRouting &r) {
    r.valid = false;

    // A patch that targets its own index is malformed: that's not a valid
    // way to express self-modulation in this model (use `feedback`
    // instead) -- routing it through the ordinary bus mechanism would need
    // exactly the block-length delay §4.2 rules out.
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (patch.op[i].mod_target == i) return false;
    }

    // Kahn's algorithm over the "i must render before mod_target[i]" edges.
    // Deterministic tie-break (lowest op index first among ready nodes) so
    // the resolved order is reproducible for testing.
    uint8_t indeg[FM_NUM_OPS] = {0, 0, 0, 0, 0, 0};
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        uint8_t t = patch.op[i].mod_target;
        if (t < FM_NUM_OPS) indeg[t]++;
    }
    bool done[FM_NUM_OPS] = {false, false, false, false, false, false};
    uint8_t count = 0;
    while (count < FM_NUM_OPS) {
        int8_t pick = -1;
        for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
            if (!done[i] && indeg[i] == 0) { pick = (int8_t)i; break; }
        }
        if (pick < 0) return false;  // multi-operator cycle: nodes remain, none ready
        done[(uint8_t)pick] = true;
        r.order[count++] = (uint8_t)pick;
        uint8_t t = patch.op[(uint8_t)pick].mod_target;
        if (t < FM_NUM_OPS) indeg[t]--;
    }

    // in_bus: operator i reads bus i (by convention, bus id == receiving
    // operator's own index) if anything targets it, else the zero bus.
    bool has_writer[FM_NUM_OPS] = {false, false, false, false, false, false};
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        uint8_t t = patch.op[i].mod_target;
        if (t < FM_NUM_OPS) has_writer[t] = true;
    }
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        r.in_bus[i] = has_writer[i] ? i : FM_BUS_ZERO;
    }

    // out_bus + kernel selection: walk the already-valid topological order,
    // so the first operator reached for a given bus really is first in
    // render order -- no separate scheduling pass needed.
    int8_t first_writer[FM_TARGET_OUT + 1];
    for (uint8_t b = 0; b <= FM_TARGET_OUT; b++) first_writer[b] = -1;
    r.clear_bus_mask = 0;
    for (uint8_t k = 0; k < FM_NUM_OPS; k++) {
        uint8_t i = r.order[k];
        uint8_t bus = patch.op[i].mod_target;  // FM_TARGET_OUT doubles as the OUT bus id
        r.out_bus[i] = bus;
        bool fb = patch.op[i].feedback;
        if (first_writer[bus] < 0) {
            first_writer[bus] = (int8_t)i;
            if (fb) {
                r.kernel[i] = FM_KERNEL_FEEDBACK;
                r.clear_bus_mask |= (uint8_t)(1u << bus);
            } else {
                r.kernel[i] = FM_KERNEL_FIRST;
            }
        } else {
            r.kernel[i] = fb ? FM_KERNEL_FEEDBACK : FM_KERNEL_PLAIN;
        }
    }

    r.valid = true;
    return true;
}

// One hardcoded 6-op patch (#44's P1 acceptance criterion), deliberately
// built to the *exact* topology #42/#43 already measured on hardware (see
// rig.h's fm_rig_render_voice_block comment) -- just expressed as
// mod_target/feedback data instead of hand-unrolled C++ calls:
//
//   op0 -> op2            (op0 unmodulated: nothing targets it)
//   op1 -> op4             (op1 unmodulated)
//   op2 -> op4             (op2 modulated by op0)
//   op3 -> op4, feedback   (self-modulated, DX7 2-sample average)
//   op4 -> op5             (modulated by op1+op2+op3 summed)
//   op5 -> OUT             (carrier, modulated by op4)
//
// fm_resolve_routing() on this patch reproduces the rig's exact bus
// assignment and kernel selection (op0/op1/op4/op5 first-writer, op2 plain
// accumulate, op3 self-feedback accumulate, no bus ever needs clearing) --
// so this patch's per-voice cost is directly comparable to the already-
// measured 100.05 c/f/voice baseline (fm.md §3.4), and the "is the emitted
// inner loop identical to #42's" acceptance criterion is checking real
// kernel reuse, not a coincidence of similar shape.
//
// Ratios: op4/op5 both ratio 1.0 is a classic 2-op FM pair (1:1 carrier:
// modulator gives a full harmonic series); op1/op2/op3 (ratios 2/3/1,
// op3 self-fed) pile three more modulators onto op4 for a denser,
// EP/bell-adjacent timbre; op0 (ratio 0.5) sub-modulates op2. Not a literal
// DX7 algorithm number -- fm.md P1 only asks for "a simple stack or a
// 2-carrier pair, not something exotic" -- but every ratio is a small
// integer (or 0.5), so the spectrum is a predictable harmonic/sideband set,
// not an inharmonic bell.
//
// Levels: NOT rig.h's MOD_GAIN/CARRIER_GAIN (1<<15/1<<16) -- those were
// deliberately scaled down for a 24-voice non-clipping bench check (rig.h's
// own comment), never meant to produce audible modulation depth. Op.h's
// verified kernel caps how much phase deviation a modulator can produce at
// all: `in[i]` is added directly to a 32-bit phase accumulator indexed by
// its top 12 bits, so 1 radian of deviation needs raw magnitude
// ≈ 2^32/2π ≈ 6.8e8, but the gain/shift chain (fm_mul_gain, >>FM_OUT_SHIFT)
// tops out around 2^24 even at gain = INT32_MAX -- roughly 0.03-0.05 rad
// max per modulator stage, since the same int32 `gain` field and shift also
// has to serve carriers at their own, much smaller, ±32767 int16-audio
// scale (`level` here is that reference ceiling -- env_dx.h's output
// level/EG/velocity-sensitivity offsets only ever attenuate *below* it).
// Modulator levels below are pushed close to that ceiling (not literally
// INT32_MAX, for headroom against rounding); the resulting sidebands are
// real, correctly-placed, and well above the noise floor (host-verified),
// but modest in amplitude -- a genuine limit of this fixed-point
// convention, unresolved by #45 (still open, see env_dx.h). Carrier (op5)
// stays at a conservative fraction of its own ±32767 unity point, leaving
// headroom for multiple summed voices plus the FX chain.
//
// EG shapes (#45): every operator gets a distinct 4-stage envelope so all
// six are audibly independent (an explicit acceptance criterion), and every
// L4 is 0 so every operator -- carrier or modulator -- actually reaches
// true silence on release (env_dx.h's EG_IDLE), not just fades toward it.
// op4/op5 (the immediate modulator:carrier pair) follow the classic DX
// electric-piano shape: both attack instantly (R1=99), but op4 (the
// modulator, i.e. the *brightness*) decays much faster and further than
// op5 (the carrier, i.e. the *loudness*) -- a bright pluck that settles
// into a mellower sustained tone, the P2 gate in fm.md §1 ("A
// DX-recognisable electric piano or bell"). Velocity sensitivity is
// highest on op4 (brightness) and op5 (loudness), lower on the other four
// -- harder hits play brighter AND louder, softer hits duller and quieter,
// exactly the DX7 EP's signature touch response.
inline constexpr FmPatch FM_TEST_PATCH = {
    "P1 Test Stack",
    {
        /* op0 */ { 0.5f, 0.0f, false, 0.0f, 400000000, 2, false,
                     99, 2, {99, 50, 20, 60}, {90, 50, 40, 0} },
        /* op1 */ { 2.0f, 0.0f, false, 0.0f, 1000000000, 4, false,
                     99, 3, {99, 70, 30, 50}, {99, 60, 55, 0} },
        /* op2 */ { 3.0f, 0.0f, false, 0.0f, 1000000000, 4, false,
                     99, 4, {95, 55, 25, 45}, {95, 45, 35, 0} },
        /* op3 */ { 1.0f, 0.0f, false, 0.0f, 1000000000, 4, true,
                     99, 3, {90, 65, 35, 55}, {99, 55, 50, 0} },
        /* op4 */ { 1.0f, 0.0f, false, 0.0f, 1400000000, 5, false,
                     99, 6, {99, 60, 20, 50}, {99, 20, 15, 0} },
        /* op5 */ { 1.0f, 0.0f, false, 0.0f, 1 << 21, FM_TARGET_OUT, false,
                     99, 5, {99, 40, 20, 40}, {99, 70, 60, 0} },
    }
};
