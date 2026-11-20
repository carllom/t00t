// Host test for the FM engine's Core 1 render (src/engines/fm/fm_render.h)
// at its seam: cooked patches published to Part slots and a voice block in,
// interleaved audio out. The reference is op.h rendering the same FmPatch
// for one voice directly, so "the same sound" means the same samples.

#include "fm_module.h"
#include "fm_render.h"

#include <cstdio>
#include <cstring>
#include <vector>

PartSlotT<FmCookedPatch> fm_part_slots[FM_PART_COUNT];
PartSlotT<FmMultiCooked> fm_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Samples = std::vector<int16_t>;

struct Values {
    int32_t v[FM_P_COUNT];
    Values() {
        for (uint32_t i = 0; i < FM_P_COUNT; i++) v[i] = FmModule::kDescriptors[i].def;
    }
    Values &set(uint32_t id, int32_t value) { v[id] = value; return *this; }
    Values &op(uint32_t op, FmOpField f, int32_t value) { return set(fm_op_param(op, f), value); }
    FmCookedPatch cook() const {
        FmCookedPatch c{};
        FmModule::cook(v, c);
        return c;
    }
};

// An electric-piano-like stack with vibrato: algorithm 5 (three pairs),
// modulators at 70, a pitch EG dip and the LFO on pitch.
Values ep() {
    Values x;
    x.set(FM_P_ALGORITHM, 4).set(FM_P_FEEDBACK, 5)
     .set(FM_P_LFO_SPEED, 60).set(FM_P_LFO_PMD, 30).set(FM_P_LFO_PMS, 5)
     .set(FM_P_PEG_L1, 40).set(FM_P_PEG_R1, 70);
    for (uint32_t i = 0; i < FM_NUM_OPS; i++) {
        x.op(i, FM_OP_LEVEL, i % 2 == 0 ? 95 : 70).op(i, FM_OP_COARSE, i % 2 == 0 ? 1 : 3)
         .op(i, FM_OP_R2, 40).op(i, FM_OP_L2, 70).op(i, FM_OP_R3, 30).op(i, FM_OP_L3, 50)
         .op(i, FM_OP_VELOCITY, 3).op(i, FM_OP_AMS, 1);
    }
    return x;
}

constexpr uint32_t kFrames = SAMPLES_PER_BUFFER;
constexpr uint32_t kBuffers = 24;
constexpr uint32_t kRelease = 12;  // the note is released at this buffer
constexpr uint8_t kNote = 60;
constexpr int16_t kAmplitude = 100 * 258;
const uint32_t kNoteInc = fm_phase_inc(261.6256f);

// --- The reference: op.h rendering one voice's FmPatch directly ---

struct Reference {
    FmOp ops[FM_NUM_OPS];
    FmPitchEg peg;
    FmLfo lfo;
    FmPatch patch;
    FmRouting routing;
    int32_t bus_mod[FM_NUM_OPS][FM_BLOCK], bus_out[FM_BLOCK];
    bool on = false;

    void start(const FmCookedPatch &c) {
        for (auto &op : ops) env_dx_reset(op.eg);
        fm_pitch_eg_init(peg);
        fm_lfo_init(lfo);
        patch = c.patch;
        fm_resolve_routing(patch, routing);
        fm_voice_note_on<DxEnvGlue>(ops, patch, kNoteInc, kAmplitude, kNote, &peg, &lfo);
        on = true;
    }
    void release() { fm_voice_note_off<DxEnvGlue>(ops, &peg); }

    void render(Samples &out) {
        int32_t l[kFrames] = {}, r[kFrames] = {};
        if (on && fm_voice_active(ops, routing)) {
            FmVoiceBuses bus{ { bus_mod[0], bus_mod[1], bus_mod[2], bus_mod[3], bus_mod[4], bus_mod[5] }, bus_out };
            fm_render_voice<FM_TABLE_BITS, DxEnvGlue>(ops, patch, routing, bus, 0, l, r, kFrames, &peg, &lfo,
                                                       kNoteInc, 0);
        }
        for (uint32_t i = 0; i < kFrames; i++) {
            out.push_back((int16_t)__ssat(l[i], 16));
            out.push_back((int16_t)__ssat(r[i], 16));
        }
    }
};

// --- The engine under test, with the Multi's FX off ---

FmRenderState g_state;  // large: keep it off the stack

struct Rig {
    VoiceParamBlock vp{};
    uint16_t load[FM_PART_COUNT] = {};
    uint16_t seq[FM_PART_COUNT] = {};

    Rig() {
        for (auto &v : vp.voices) v = voice_params_default<VoiceParams>();
        fm_render_init(g_state);
        FmCookedPatch drain;
        for (auto &s : fm_part_slots) s.consume(drain);
        FmMultiCooked mc = multi_cooked_default<FM_PART_COUNT>();
        mc.fx.type = FX_OFF;
        g_state.multi = mc;
    }

    // Core 0's side: a new Patch for `part`, or an edit of the one it has.
    void load_patch(uint8_t part, const FmCookedPatch &c) { publish(part, c, ++load[part]); }
    void edit_patch(uint8_t part, const FmCookedPatch &c) { publish(part, c, load[part]); }
    void publish(uint8_t part, const FmCookedPatch &c, uint16_t load_id) {
        check(fm_part_slots[part].try_publish(c, load_id, ++seq[part]), "the Part slot accepts the publish");
    }

    // A note on `part` with the Part's latest cook (seq_ahead: a cook Core 0
    // has made but not yet published).
    void note_on(uint8_t v, uint8_t part, uint16_t seq_ahead = 0) {
        VoiceParams &p = vp.voices[v];
        p.phase_inc = kNoteInc;
        p.amplitude = kAmplitude;
        p.note = kNote;
        p.part = part;
        p.load_id = load[part];
        p.cook_seq = (uint16_t)(seq[part] + seq_ahead);
        p.mod_wheel = 0;
        p.trigger++;
        p.gate = true;
    }
    void note_off(uint8_t v) { vp.voices[v].gate = false; }

    void render(Samples &out) {
        for (uint32_t p = 0; p < FM_PART_COUNT; p++) fm_render_take_patch(g_state, p, fm_part_slots[p]);
        int16_t buf[kFrames * 2];
        fm_render_buffer(g_state, vp, buf);
        out.insert(out.end(), buf, buf + kFrames * 2);
    }
};

bool silent(const Samples &s, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) if (s[i]) return false;
    return true;
}

// --- Tests ---

void test_a_note_sounds_as_op_h_renders_its_patch() {
    Rig rig;
    const FmCookedPatch c = ep().cook();
    rig.load_patch(0, c);
    rig.note_on(0, 0);
    Reference ref;
    ref.start(c);
    Samples got, want;
    for (uint32_t b = 0; b < kBuffers; b++) {
        if (b == kRelease) { rig.note_off(0); ref.release(); }
        rig.render(got);
        ref.render(want);
    }
    check(!silent(want, 0, want.size()), "the reference renders sound");
    check(got == want, "a note on the cooked path renders the same samples as op.h with the patch");
}

void test_a_live_edit_reaches_a_sounding_note() {
    Rig rig;
    const FmCookedPatch a = ep().cook();
    const FmCookedPatch more_vibrato = ep().set(FM_P_LFO_PMD, 90).set(FM_P_PEG_L3, 60).cook();
    rig.load_patch(0, a);
    rig.note_on(0, 0);
    Reference ref, unedited;
    ref.start(a);
    unedited.start(a);
    Samples got, want, plain;
    for (uint32_t b = 0; b < kBuffers; b++) {
        if (b == 6) {
            rig.edit_patch(0, more_vibrato);
            ref.patch.lfo.pmd = 90;
            ref.patch.pitch_eg.level[2] = 60;
        }
        rig.render(got);
        ref.render(want);
        unedited.render(plain);
    }
    check(got != plain, "an LFO and pitch EG edit changes the sounding note");
    check(got == want, "from the buffer it arrives in, exactly as if the note had those values from then on");
}

void test_a_note_on_edit_waits_for_the_next_note() {
    Rig rig;
    const FmCookedPatch a = ep().cook();
    const FmCookedPatch b = ep().set(FM_P_ALGORITHM, 31).op(1, FM_OP_COARSE, 7).cook();
    rig.load_patch(0, a);
    rig.note_on(0, 0);
    Reference ref_a;
    ref_a.start(a);
    Samples got, want;
    for (uint32_t i = 0; i < 8; i++) {
        if (i == 3) rig.edit_patch(0, b);
        rig.render(got);
        ref_a.render(want);
    }
    check(got == want, "an algorithm and frequency edit leaves the sounding note as it started");

    rig.note_off(0);
    Samples tail;
    for (uint32_t i = 0; i < 50 && (g_state.active_mask & 1u); i++) rig.render(tail);
    rig.note_on(1, 0);
    Reference ref_b;
    ref_b.start(b);
    Samples next, want_next;
    for (uint32_t i = 0; i < 6; i++) {
        rig.render(next);
        ref_b.render(want_next);
    }
    check(!(g_state.active_mask & 1u) && next == want_next, "the next note plays the edited algorithm and frequency");
}

void test_a_new_patch_leaves_a_sounding_note_on_its_own() {
    Rig rig;
    const FmCookedPatch a = ep().cook();
    const FmCookedPatch b = ep().set(FM_P_ALGORITHM, 31).set(FM_P_LFO_PMD, 99).cook();
    rig.load_patch(0, a);
    rig.note_on(0, 0);
    Reference ref;
    ref.start(a);
    Samples got, want;
    for (uint32_t i = 0; i < 10; i++) {
        if (i == 3) rig.load_patch(0, b);
        if (i == 6) rig.edit_patch(0, ep().set(FM_P_LFO_PMD, 0).cook());  // an edit of the new Patch
        rig.render(got);
        ref.render(want);
    }
    check(got == want, "a Program Change leaves the held note on its Patch, and that Patch's edits don't reach it");
}

void test_a_note_waits_for_its_cook() {
    Rig rig;
    const FmCookedPatch a = ep().cook();
    const FmCookedPatch b = ep().set(FM_P_ALGORITHM, 31).cook();
    rig.load_patch(0, a);
    rig.note_on(0, 0, 1);  // played after an edit whose publish is still pending
    Samples got;
    rig.render(got);
    check(silent(got, 0, got.size()), "the note waits while its cook is still on Core 0");
    rig.edit_patch(0, b);
    Reference ref;
    ref.start(b);
    Samples want;
    want.assign(kFrames * 2, 0);
    for (uint32_t i = 0; i < 6; i++) {
        rig.render(got);
        ref.render(want);
    }
    check(got == want, "and starts a buffer later on that cook");
}

void test_the_part_mix_pans_and_scales_the_note() {
    Rig rig;
    const FmCookedPatch a = ep().cook();
    rig.load_patch(2, a);
    rig.note_on(0, 2);
    g_state.multi.part[2].pan_q15 = -32768;  // hard left
    Samples got;
    for (uint32_t i = 0; i < 4; i++) rig.render(got);
    bool right_silent = true, left_sounds = false;
    for (size_t i = 0; i < got.size(); i += 2) {
        left_sounds = left_sounds || got[i] != 0;
        right_silent = right_silent && got[i + 1] == 0;
    }
    check(left_sounds && right_silent, "a Part panned hard left sounds only on the left");

    Rig quiet;
    quiet.load_patch(2, a);
    quiet.note_on(0, 2);
    g_state.multi.part[2].volume_q15 = 0;
    Samples q;
    for (uint32_t i = 0; i < 4; i++) quiet.render(q);
    check(silent(q, 0, q.size()), "a Part at volume 0 is silent");
}

void test_a_note_released_while_waiting_for_its_cook_ends() {
    Rig rig;
    rig.load_patch(0, ep().cook());
    rig.note_on(0, 0, 1);  // its cook is still on Core 0
    Samples got;
    rig.render(got);
    rig.note_off(0);       // released before it ever started
    rig.edit_patch(0, ep().cook());
    for (uint32_t i = 0; i < 200 && (i == 0 || (g_state.active_mask & 1u)); i++) rig.render(got);
    check(!(g_state.active_mask & 1u), "it starts, releases at once and ends");
}

void test_a_note_with_every_carrier_off_is_silent() {
    Rig rig;
    rig.load_patch(0, ep().cook());
    Values off = ep();
    for (uint32_t i = 0; i < FM_NUM_OPS; i += 2) off.op(i, FM_OP_ENABLE, 0);  // algorithm 5's carriers
    rig.load_patch(1, off.cook());
    rig.note_on(0, 0);
    rig.note_on(1, 1);
    Samples both;
    for (uint32_t i = 0; i < 4; i++) rig.render(both);

    Rig alone_rig;
    alone_rig.load_patch(0, ep().cook());
    alone_rig.note_on(0, 0);
    Samples alone;
    for (uint32_t i = 0; i < 4; i++) alone_rig.render(alone);
    check(both == alone, "it adds nothing to the mix, not another voice's leftover output");
    check(!(g_state.active_mask & 2u), "and its voice isn't sounding");
}

}  // namespace

int main() {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();
    test_a_note_sounds_as_op_h_renders_its_patch();
    test_a_live_edit_reaches_a_sounding_note();
    test_a_note_on_edit_waits_for_the_next_note();
    test_a_new_patch_leaves_a_sounding_note_on_its_own();
    test_a_note_waits_for_its_cook();
    test_the_part_mix_pans_and_scales_the_note();
    test_a_note_released_while_waiting_for_its_cook_ends();
    test_a_note_with_every_carrier_off_is_silent();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
