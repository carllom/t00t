#pragma once

#include <cstdint>

// FM patch data + note-on-time routing compiler (#44, module_fm.md §4/§5.6/§7):
// the runtime FmOpParams/FmPatch shape the eventual tools/syx2patch.py
// converter (P3) will emit, plus the DAG-routing resolver that turns one of
// those patches into per-operator order/bus/kernel decisions -- module_fm.md §4.1's
// central claim, "an operator's routing IS its in/out bus pointers plus its
// position in the processing order, both resolved once at note-on," lives
// here. No pico-sdk dependency (plain cstdint), so this header is shared by
// both the device engine (audio_engine.cpp/midi_controller.cpp) and the host
// render/test harness (tools/host_render/render_fm.cpp), matching the
// render.h/sine_tab.h/rig.h convention already established for this engine.

static constexpr uint8_t FM_NUM_OPS = 6;

// Bus id space (module_fm.md §4.3): 0-5 are the six operator-indexed modulation
// buses (bus b is "the bus operator b reads as its own modulation input"),
// FM_TARGET_OUT is the shared voice output bus that carriers sum into, and
// FM_BUS_ZERO is a read-only all-zero source for operators nothing
// modulates (module_fm.md §5.2: "pure carriers pointing at a zero bus"). The same
// numeric value doubles as FmOpParams::mod_target's "this op is a carrier"
// sentinel and as the routing compiler's output-bus id -- a carrier's
// output IS the thing being routed to bus FM_TARGET_OUT, so one constant
// serves both roles without a separate mapping step.
static constexpr uint8_t FM_TARGET_OUT = FM_NUM_OPS;      // 6
static constexpr uint8_t FM_BUS_ZERO   = FM_NUM_OPS + 1;  // 7

// One operator's patch data -- what tools/syx2patch.py emits per DX7
// operator (module_fm.md §7's table): ratio/detune/fixed-frequency mode resolve to
// the Q32 increment (§5.6), and `mod_target`/`feedback_level` together are
// the entire routing input: which operator (or FM_TARGET_OUT) this op's
// output feeds, and how strongly (0-7, DX7 units) its own last two outputs
// additionally self-modulate its phase. `output_level`/`vel_sensitivity`/
// `eg_rate`/`eg_level` plus the key-scaling fields (env_dx.h) are the
// note-on/block-rate-resolved pieces that produce the actual, time-varying
// `gain` op_render sees.
//
// This struct has no per-operator "reference gain" field. There is one
// engine-wide ceiling (op.h's FM_GAIN_MAX, fm_scale.h) and everything below
// it is DX7 parameters -- output level, EG level, key scaling, velocity --
// attenuating in the log domain, same as real hardware. Nothing outside
// op.h/fm_scale.h is allowed an opinion about absolute level.
struct FmOpParams {
    float   ratio;         // coarse.fine frequency ratio against the note (ignored if fixed_freq)
    float   fixed_hz;      // absolute frequency in Hz, used only when fixed_freq is true
    bool    fixed_freq;
    // Raw DX7 detune, offset so 0 == neutral (DX7 byte 0-14 minus 7, so
    // -7..+7). F5 changed this from a baked cents value: real DX7 detune is
    // note-dependent in ratio mode and a different, sharpen-only rule in fixed
    // mode, so it cannot be resolved by a note-independent converter. op.h's
    // fm_op_base_inc() applies both rules at note-on.
    int8_t  detune_offset;
    uint8_t mod_target;    // 0..FM_NUM_OPS-1 (another operator), or FM_TARGET_OUT (carrier)
    // The operators BESIDES mod_target that this operator also modulates, as
    // a bitmask of operator indices. 0 for all but 7 of the 32 DX7
    // algorithms -- `mod_target` alone cannot express algorithms 19-25,
    // where one modulator drives two or three carriers at once (Dexed's OP6
    // writes a scratch bus and OP5, OP4 and OP3 each read it, without the bus
    // being cleared in between).
    //
    // Fan-out is kept as a mask on the SOURCE, rather than switching the bus
    // convention from receiver-indexed to source-indexed, because fan-IN
    // (algorithm 7 sums OP5 and OP4 into OP3's modulation, and up to three
    // operators elsewhere) needs the receiver-indexed form to work at all --
    // the additive second writer relies on `bus id == receiving operator`.
    // The two shapes never collide: across all 32 algorithms, no operator that
    // is the target of a fan-out ever has a second modulator (checked
    // exhaustively by `tools/fm_ctl_diff.py`'s `table/routing` case, which
    // reconstructs Dexed's real bus semantics). So an extra target's `in_bus`
    // can point straight at its single source's bus with no ambiguity.
    uint8_t extra_target_mask;
    // Self-modulation depth, DX7 units (0-7; 0 = off) -- real hardware spans
    // a 64x (2^6) depth range across levels 1-7, not a simple on/off switch
    // (dx7note.cc's `fb_shift_ = feedback ? 8-feedback : 16`,
    // fm_op_kernel.cc's `compute_fb`). See op.h's op_render_fb for how this
    // resolves into FmRouting::fb_shift at note-on.
    uint8_t feedback_level;
    uint8_t output_level;    // DX7 TL, 0-99 -- folded into the EG's own stage targets (env_dx.h)
    uint8_t vel_sensitivity; // 0-7, env_dx.h's dx7_scale_velocity()
    uint8_t eg_rate[4];      // R1-R4, 0-99 (#45)
    uint8_t eg_level[4];     // L1-L4, 0-99 (#45) -- L4 = 0 lets this operator actually reach silence on release
    // #48: key level scaling -- an output-level offset that depends on the
    // played note vs. `scale_breakpoint`, resolved once at note-on
    // (env_dx.h's dx7_scale_level(), a direct Dexed port) and folded into
    // the EG's `outlevel` alongside output_level/vel_sensitivity. curve values
    // match DX7: 0=-LIN, 1=-EXP, 2=+EXP, 3=+LIN (env_dx.h's dx7_scale_curve()).
    uint8_t scale_breakpoint;  // MIDI note, DX7 units (C3 = 0x27 = 39)
    uint8_t scale_left_depth;  // 0-99
    uint8_t scale_right_depth; // 0-99
    uint8_t scale_left_curve;  // 0-3
    uint8_t scale_right_curve; // 0-3
    // #48: rate scaling -- higher notes get faster envelopes. 0-7,
    // env_dx.h's dx7_scale_rate() (a direct Dexed port), resolved once at
    // note-on into a qrate delta applied at every stage transition
    // (env_dx_step_block).
    uint8_t rate_scaling;
    // #49: amplitude modulation sensitivity -- how much this operator's own
    // gain dips under the voice's shared LFO tremolo (lfo.h's `amd`/
    // waveform), 0-3 (DX7 AMS is a 2-bit field, unlike PMS's 0-7). 0 is "not
    // affected" -- a real, common, and SAFE default (see patch.h's own
    // FM_TEST_PATCH comment on why `pitch_eg`'s default is NOT similarly
    // safe at zero).
    uint8_t am_sensitivity;
};

// #49 (module_fm.md §5.5): one LFO per VOICE (not per operator) -- rate/delay/
// waveform/depths/sensitivity are shared by all six operators, only each
// operator's own `am_sensitivity` (FmOpParams, above) varies how much the
// shared tremolo reaches it. Field values and ranges match DX7 exactly
// (fm/lfo.h ports Dexed's own Lfo class, Source/msfa/lfo.cc, Apache-2.0) so
// tools/syx2patch.py can copy them straight through, the same convention
// #45/#47 already established for eg_rate/eg_level/output_level.
struct FmLfoParams {
    uint8_t rate;      // 0-99, DX7 LFO speed (lfo.h's dx7_lfo_rate_to_hz(): ~0.06 Hz at 0, ~51 Hz at 99)
    uint8_t delay;      // 0-99, DX7 LFO delay (fade-in after note-on, ~0-2.6s)
    uint8_t pmd;        // 0-99, pitch mod depth
    uint8_t amd;        // 0-99, amp mod depth
    uint8_t waveform;    // 0=triangle, 1=saw down, 2=saw up, 3=square, 4=sine, 5=sample & hold (DX7 numbering)
    bool    key_sync;    // true: this voice's LFO phase resets at every note-on
    uint8_t pms;         // 0-7, pitch mod sensitivity (voice-wide; separate from each op's own am_sensitivity)
};

// #49 (module_fm.md §5.4): one 4-stage (rate, level) pitch envelope per VOICE, same
// shape as EnvDX (env_dx.h) but in a cents domain and shared by all six
// operators (pitch_eg.h scales every non-fixed-frequency operator's `inc`
// by the same ratio each control block). **level 50 is "no deviation" (DX7
// hardware center), NOT level 0** -- unlike every other 0-99 DX7 field in
// this file, a zero-initialized `FmPitchEgParams` is a real, audible ~4-
// octave pitch DROP on every stage, not a safe "off" default (see
// pitch_eg.h's own comment; the same "zero-init is not idle" pitfall
// env_dx.h's own EnvDX already warns about, just for patch DATA here
// instead of runtime state). Every hand-written FmPatch literal in this
// codebase (FM_TEST_PATCH below, and anything copy-constructed from it)
// MUST set `level` to {50,50,50,50} explicitly to mean "no pitch EG effect"
// -- tools/syx2patch.py never hits this pitfall since it always copies a
// real patch's real level bytes straight through, verbatim, the same as
// every other DX7 field.
struct FmPitchEgParams {
    uint8_t rate[4];   // R1-R4, 0-99
    uint8_t level[4];  // L1-L4, 0-99 -- 50 = center/no deviation, NOT 0
};

struct FmPatch {
    const char *name;
    FmOpParams  op[FM_NUM_OPS];
    FmLfoParams      lfo;       // #49 -- voice-wide, shared by all six operators
    FmPitchEgParams  pitch_eg;  // #49 -- voice-wide, shared by all six operators
};

// Note-on-time routing decisions (module_fm.md §5.6): everything the per-sample
// kernel needs, with nothing in it that depends on note/velocity/bend --
// only on the patch. `order` is the topological processing order;
// `in_bus`/`out_bus` are per-operator bus ids (0-5, FM_TARGET_OUT, or
// FM_BUS_ZERO for in_bus only); `kernel` selects one of op.h's three
// variants; `clear_bus_mask` (bit b = bus b, b in 0..FM_TARGET_OUT) flags
// the rare bus whose only writer is a feedback operator -- op_render_fb
// always accumulates (module_fm.md §5.2), so if that's the *first* write to a bus
// it must be pre-zeroed rather than relying on the usual "no bus ever needs
// clearing" first-writer optimization (§4.3).
enum FmKernel : uint8_t { FM_KERNEL_FIRST, FM_KERNEL_PLAIN, FM_KERNEL_FEEDBACK };

struct FmRouting {
    uint8_t order[FM_NUM_OPS];
    uint8_t kernel[FM_NUM_OPS];
    uint8_t in_bus[FM_NUM_OPS];
    uint8_t out_bus[FM_NUM_OPS];
    uint8_t clear_bus_mask;
    // `op_render_fb` (op.h) applies the TOTAL right-shift `fb_shift + 1` to
    // (fb1+fb2) before feeding it back into phase -- the `+1` matches
    // Dexed's own `compute_fb` (`scaled_fb = (y0 + y) >> (fb_shift + 1)`)
    // exactly; a bare `>> fb_shift` here would be 2x too much feedback at
    // every level. Resolved once here from FmOpParams::feedback_level as
    // `8 - feedback_level`, matching Dexed's own per-level spacing
    // (dx7note.cc's `fb_shift_ = 8 - feedback`) -- level 7 (max) gives
    // `fb_shift = 1`, so a total shift of `>>2`; each level below steps the
    // total shift up by one octave. Only meaningful where kernel[i] ==
    // FM_KERNEL_FEEDBACK.
    uint8_t fb_shift[FM_NUM_OPS];
    bool    valid;
};

// Resolves `patch` into `r`. Returns false (r.valid = false) if the patch's
// mod_target graph contains a cycle spanning two or more operators -- the
// one routing shape block-inner rendering can't evaluate without a
// block-length delay (module_fm.md §4.2). Self-modulation (`feedback_level` > 0) is
// accepted unconditionally: it never enters this graph at all, because it's
// satisfied entirely inside op_render_fb's own per-sample fb1/fb2 history,
// not by bus-write ordering -- so there is nothing for a cycle check to
// reject.
inline bool fm_resolve_routing(const FmPatch &patch, FmRouting &r) {
    r.valid = false;

    // A patch that targets its own index is malformed: that's not a valid
    // way to express self-modulation in this model (use `feedback_level`
    // instead) -- routing it through the ordinary bus mechanism would need
    // exactly the block-length delay §4.2 rules out.
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (patch.op[i].mod_target == i) return false;
        // Same rule for the fan-out mask: an operator may not name itself, and
        // may not repeat its own primary target (which would double-count the
        // edge in the topological sort and never reach zero in-degree).
        if (patch.op[i].extra_target_mask & (1u << i)) return false;
        if (patch.op[i].mod_target < FM_NUM_OPS &&
            (patch.op[i].extra_target_mask & (1u << patch.op[i].mod_target))) return false;
        // A carrier has no bus of its own for extra targets to read.
        if (patch.op[i].extra_target_mask && patch.op[i].mod_target >= FM_NUM_OPS) return false;
    }

    // Kahn's algorithm over the "i must render before mod_target[i]" edges.
    // Deterministic tie-break (lowest op index first among ready nodes) so
    // the resolved order is reproducible for testing.
    uint8_t indeg[FM_NUM_OPS] = {0, 0, 0, 0, 0, 0};
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        uint8_t t = patch.op[i].mod_target;
        if (t < FM_NUM_OPS) indeg[t]++;
        // Extra fan-out targets are real edges too: they must render after
        // their source, exactly like the primary target.
        for (uint8_t j = 0; j < FM_NUM_OPS; j++) {
            if (patch.op[i].extra_target_mask & (1u << j)) indeg[j]++;
        }
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
        for (uint8_t j = 0; j < FM_NUM_OPS; j++) {
            if (patch.op[(uint8_t)pick].extra_target_mask & (1u << j)) indeg[j]--;
        }
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
    // An extra fan-out target reads its SOURCE's bus rather than its own,
    // since nothing writes a bus named after it. Safe precisely because such a
    // target has no second modulator (see FmOpParams::extra_target_mask), so
    // this can never overwrite a fan-in arrangement.
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (!patch.op[i].extra_target_mask) continue;
        uint8_t src_bus = patch.op[i].mod_target;
        if (src_bus > FM_NUM_OPS) continue;  // malformed; the checks above already reject it
        for (uint8_t j = 0; j < FM_NUM_OPS; j++) {
            if (patch.op[i].extra_target_mask & (1u << j)) r.in_bus[j] = src_bus;
        }
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
        uint8_t level = patch.op[i].feedback_level;
        bool fb = level > 0;
        if (fb) r.fb_shift[i] = (uint8_t)(8 - level);
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
// mod_target/feedback_level data instead of hand-unrolled C++ calls:
//
//   op0 -> op2                    (op0 unmodulated: nothing targets it)
//   op1 -> op4                     (op1 unmodulated)
//   op2 -> op4                     (op2 modulated by op0)
//   op3 -> op4, feedback_level=7   (self-modulated, DX7 2-sample average, max depth)
//   op4 -> op5                     (modulated by op1+op2+op3 summed)
//   op5 -> OUT             (carrier, modulated by op4)
//
// fm_resolve_routing() on this patch reproduces the rig's exact bus
// assignment and kernel selection (op0/op1/op4/op5 first-writer, op2 plain
// accumulate, op3 self-feedback accumulate, no bus ever needs clearing) --
// so this patch's per-voice cost is directly comparable to the already-
// measured 100.05 c/f/voice baseline (module_fm.md §3.4), and the "is the emitted
// inner loop identical to #42's" acceptance criterion is checking real
// kernel reuse, not a coincidence of similar shape.
//
// Ratios: op4/op5 both ratio 1.0 is a classic 2-op FM pair (1:1 carrier:
// modulator gives a full harmonic series); op1/op2/op3 (ratios 2/3/1,
// op3 self-fed) pile three more modulators onto op4 for a denser,
// EP/bell-adjacent timbre; op0 (ratio 0.5) sub-modulates op2. Not a literal
// DX7 algorithm number -- module_fm.md P1 only asks for "a simple stack or a
// 2-carrier pair, not something exotic" -- but every ratio is a small
// integer (or 0.5), so the spectrum is a predictable harmonic/sideband set,
// not an inharmonic bell.
//
// Levels: loudness is `output_level` alone, in real DX7 units, against
// op.h's single engine-wide ceiling -- a modulator at output_level 99 with
// its EG open produces two full cycles of phase deviation on its target,
// exactly as a max-level DX7 operator does. The values below are ordinary
// DX7 voicing -- carrier at 99, modulators in the 75-88 range real patches
// use -- reasoned about and changed the same way a real DX7 patch sheet
// would be, with no engine-side scaling to account for.
//
// EG shapes: every operator gets a distinct 4-stage envelope so all
// six are audibly independent (an explicit acceptance criterion), and every
// L4 is 0 so every operator -- carrier or modulator -- actually reaches
// true silence on release (env_dx.h's EG_IDLE), not just fades toward it.
// op4/op5 (the immediate modulator:carrier pair) follow the classic DX
// electric-piano shape: both attack instantly (R1=99), but op4 (the
// modulator, i.e. the *brightness*) decays much faster and further than
// op5 (the carrier, i.e. the *loudness*) -- a bright pluck that settles
// into a mellower sustained tone, the P2 gate in module_fm.md §1 ("A
// DX-recognisable electric piano or bell"). Velocity sensitivity is
// highest on op4 (brightness) and op5 (loudness), lower on the other four
// -- harder hits play brighter AND louder, softer hits duller and quieter,
// exactly the DX7 EP's signature touch response.
inline constexpr FmPatch FM_TEST_PATCH = {
    "P1 Test Stack",
    {
        // F7 added `extra_target_mask` after `mod_target`; it is 0 for every
        // operator here, since this patch is a plain single-target chain.
        /* op0 */ { 0.5f, 0.0f, false,  0, 2, 0, false,
                     75, 2, {99, 50, 20, 60}, {90, 50, 40, 0} },
        /* op1 */ { 2.0f, 0.0f, false,  0, 4, 0, false,
                     80, 3, {99, 70, 30, 50}, {99, 60, 55, 0} },
        /* op2 */ { 3.0f, 0.0f, false,  0, 4, 0, false,
                     78, 4, {95, 55, 25, 45}, {95, 45, 35, 0} },
        /* op3 */ { 1.0f, 0.0f, false,  0, 4, 0, 7,
                     82, 3, {90, 65, 35, 55}, {99, 55, 50, 0} },
        /* op4 */ { 1.0f, 0.0f, false,  0, 5, 0, false,
                     88, 6, {99, 60, 20, 50}, {99, 20, 15, 0} },
        /* op5 */ { 1.0f, 0.0f, false,  0, FM_TARGET_OUT, 0, false,
                     99, 5, {99, 40, 20, 40}, {99, 70, 60, 0} },
    },
    // #49: lfo.pmd/amd = 0 leaves this patch's sound identical to every
    // pre-#49 render (0 depth means the LFO is stepped but contributes
    // exactly 0 cents/0 attenuation regardless of rate/waveform/delay, so
    // those three are left at a plausible "moderate vibrato, if it were
    // ever turned on" setting rather than needing to be zero too). pitch_eg
    // MUST set level to {50,50,50,50} explicitly -- see FmPitchEgParams'
    // own comment; zero would be a real ~4-octave pitch drop, not "off".
    /* lfo       */ { 35, 0, 0, 0, /*waveform=*/4, /*key_sync=*/true, /*pms=*/0 },
    /* pitch_eg  */ { {50, 50, 50, 50}, {50, 50, 50, 50} },
};
