// Host test for the OPL engine's Core 1 render (src/engines/opl/
// opl_render.h) at its seam: cooked patches published to Part slots and a
// voice block in, interleaved audio out. The reference is opl_voice.h
// rendering the same OplPatch for one voice directly, as the engine did
// before the patch platform, so "the same sound" means the same samples.

#include "opl_module.h"
#include "opl_render.h"
#include "patches.h"

#include <cstdio>
#include <vector>

PartSlotT<OplCookedPatch> opl_part_slots[OPL_PART_COUNT];
PartSlotT<OplMultiCooked> opl_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Samples = std::vector<int16_t>;

// `patch` as the platform cooks it, from its register values.
OplCookedPatch cook(const OplPatch &patch) {
    int32_t v[OPL_P_COUNT];
    for (uint32_t o = 0; o < OPL_NUM_OPS; o++) {
        const OplOpParams &op = patch.op[o];
        v[opl_op_param(o, OPL_OP_MULT)] = op.mult;
        v[opl_op_param(o, OPL_OP_KSL)] = op.ksl;
        v[opl_op_param(o, OPL_OP_TL)] = op.tl;
        v[opl_op_param(o, OPL_OP_AR)] = op.ar;
        v[opl_op_param(o, OPL_OP_DR)] = op.dr;
        v[opl_op_param(o, OPL_OP_SL)] = op.sl;
        v[opl_op_param(o, OPL_OP_RR)] = op.rr;
        v[opl_op_param(o, OPL_OP_EGT)] = op.egt;
        v[opl_op_param(o, OPL_OP_KSR)] = op.ksr;
        v[opl_op_param(o, OPL_OP_WAVE)] = op.ws;
    }
    v[OPL_P_ALGORITHM] = patch.algorithm;
    v[OPL_P_FEEDBACK] = patch.feedback;
    OplCookedPatch c{};
    OplModule::cook(v, c);
    return c;
}

constexpr uint32_t kFrames = SAMPLES_PER_BUFFER;
constexpr uint32_t kBuffers = 24;
constexpr uint32_t kRelease = 12;  // the note is released at this buffer
constexpr uint8_t kNote = 60;
constexpr int16_t kAmplitude = 100 * 258;
constexpr int16_t kModWheel = 20000;  // some vibrato
const uint32_t kNoteInc = fm_phase_inc(261.6256f);

// --- The reference: opl_voice.h rendering one voice's OplPatch directly ---

struct Reference {
    FmOp ops[FM_NUM_OPS];
    EnvOpl env[OPL_NUM_OPS];
    FmRouting routing;
    OplVibrato vib;
    OplPatch patch;
    int32_t bus_mod[FM_NUM_OPS][FM_BLOCK], bus_out[FM_BLOCK];
    bool on = false;

    void start(const OplPatch &p) {
        opl_voice_init_inert(ops);
        patch = p;
        opl_voice_note_on(ops, env, routing, patch, kNoteInc, kAmplitude, kNote, vib);
        on = true;
    }
    void release() { opl_voice_note_off(env, routing); }

    void render(Samples &out) {
        int32_t l[kFrames] = {}, r[kFrames] = {};
        if (on && opl_voice_active(env, routing)) {
            FmVoiceBuses bus{ { bus_mod[0], bus_mod[1], bus_mod[2], bus_mod[3], bus_mod[4], bus_mod[5] }, bus_out };
            opl_render_voice(ops, env, routing, bus, 0, l, r, kFrames, vib, kModWheel);
        }
        for (uint32_t i = 0; i < kFrames; i++) {
            out.push_back((int16_t)__ssat(l[i], 16));
            out.push_back((int16_t)__ssat(r[i], 16));
        }
    }
};

// --- The engine under test, with the Multi's FX off ---

OplRenderState g_state;  // large: keep it off the stack

struct Rig {
    VoiceParamBlock vp{};
    uint16_t load[OPL_PART_COUNT] = {};
    uint16_t seq[OPL_PART_COUNT] = {};

    Rig() {
        for (auto &v : vp.voices) v = voice_params_default<VoiceParams>();
        opl_render_init(g_state);
        OplCookedPatch drain;
        for (auto &s : opl_part_slots) s.consume(drain);
        OplMultiCooked mc = multi_cooked_default<OPL_PART_COUNT>();
        mc.fx.type = FX_OFF;
        g_state.multi = mc;
    }

    // Core 0's side: a new Patch for `part`, or an edit of the one it has.
    void load_patch(uint8_t part, const OplCookedPatch &c) { publish(part, c, ++load[part]); }
    void edit_patch(uint8_t part, const OplCookedPatch &c) { publish(part, c, load[part]); }
    void publish(uint8_t part, const OplCookedPatch &c, uint16_t load_id) {
        check(opl_part_slots[part].try_publish(c, load_id, ++seq[part]), "the Part slot accepts the publish");
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
        p.mod_wheel = kModWheel;
        p.trigger++;
        p.gate = true;
    }
    void note_off(uint8_t v) { vp.voices[v].gate = false; }

    void render(Samples &out) {
        for (uint32_t p = 0; p < OPL_PART_COUNT; p++) opl_render_take_patch(g_state, p, opl_part_slots[p]);
        int16_t buf[kFrames * 2];
        opl_render_buffer(g_state, vp, buf);
        out.insert(out.end(), buf, buf + kFrames * 2);
    }
};

bool silent(const Samples &s, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) if (s[i]) return false;
    return true;
}

// --- Tests ---

void test_every_factory_patch_sounds_as_opl_voice_renders_it() {
    bool all = true, any_silent = false;
    for (uint32_t k = 0; k < OPL_PATCH_COUNT; k++) {
        Rig rig;
        rig.load_patch(0, cook(*OPL_PATCHES[k]));
        rig.note_on(0, 0);
        Reference ref;
        ref.start(*OPL_PATCHES[k]);
        Samples got, want;
        for (uint32_t b = 0; b < kBuffers; b++) {
            if (b == kRelease) { rig.note_off(0); ref.release(); }
            rig.render(got);
            ref.render(want);
        }
        any_silent = any_silent || silent(want, 0, want.size());
        if (got != want) {
            printf("     %s differs\n", OPL_PATCHES[k]->name);
            all = false;
        }
    }
    check(!any_silent, "the reference renders sound for every patch");
    check(all, "every patches.h patch on the cooked path renders the same samples as opl_voice.h");
}

void test_an_edit_waits_for_the_next_note() {
    Rig rig;
    OplPatch edited = OPL_PATCH_LEAD;
    edited.algorithm = OPL_ALGO_ADD;
    edited.op[0].mult = 7;
    rig.load_patch(0, cook(OPL_PATCH_LEAD));
    rig.note_on(0, 0);
    Reference ref_a;
    ref_a.start(OPL_PATCH_LEAD);
    Samples got, want;
    for (uint32_t i = 0; i < 8; i++) {
        if (i == 3) rig.edit_patch(0, cook(edited));
        rig.render(got);
        ref_a.render(want);
    }
    check(got == want, "an algorithm and multiplier edit leaves the sounding note as it started");

    rig.note_off(0);
    Samples tail;
    for (uint32_t i = 0; i < 60 && (g_state.active_mask & 1u); i++) rig.render(tail);
    rig.note_on(1, 0);
    Reference ref_b;
    ref_b.start(edited);
    Samples next, want_next;
    for (uint32_t i = 0; i < 6; i++) {
        rig.render(next);
        ref_b.render(want_next);
    }
    check(!(g_state.active_mask & 1u) && !silent(want_next, 0, want_next.size()) && next == want_next,
          "the next note plays the edited algorithm and multiplier");
}

void test_a_new_patch_leaves_a_sounding_note_on_its_own() {
    Rig rig;
    rig.load_patch(0, cook(OPL_PATCH_LEAD));
    rig.note_on(0, 0);
    Reference ref;
    ref.start(OPL_PATCH_LEAD);
    Samples got, want;
    for (uint32_t i = 0; i < 10; i++) {
        if (i == 3) rig.load_patch(0, cook(OPL4_PATCH_BRASS));
        rig.render(got);
        ref.render(want);
    }
    check(got == want, "a Program Change leaves the held note on its Patch");
}

void test_a_note_waits_for_its_cook() {
    Rig rig;
    rig.load_patch(0, cook(OPL_PATCH_LEAD));
    rig.note_on(0, 0, 1);  // played after an edit whose publish is still pending
    Samples got;
    rig.render(got);
    check(silent(got, 0, got.size()), "the note waits while its cook is still on Core 0");
    rig.edit_patch(0, cook(OPL_PATCH_BELL));
    Reference ref;
    ref.start(OPL_PATCH_BELL);
    Samples want;
    want.assign(kFrames * 2, 0);
    for (uint32_t i = 0; i < 6; i++) {
        rig.render(got);
        ref.render(want);
    }
    check(got == want, "and starts a buffer later on that cook");
}

void test_a_note_played_just_before_a_program_change_keeps_its_patch() {
    Rig rig;
    rig.load_patch(0, cook(OPL_PATCH_LEAD));
    Samples got;
    rig.render(got);
    // Core 0: the note, then a Program Change, both before Core 1's next buffer.
    rig.note_on(0, 0);
    rig.load_patch(0, cook(OPL4_PATCH_BRASS));
    Reference ref;
    ref.start(OPL_PATCH_LEAD);
    got.clear();
    Samples want;
    for (uint32_t i = 0; i < 6; i++) {
        rig.render(got);
        ref.render(want);
    }
    check(got == want, "it starts on the Part's previous cook, the Patch it was played on");
}

void test_the_part_mix_pans_and_scales_the_note() {
    Rig rig;
    rig.load_patch(2, cook(OPL_PATCH_ORGAN));
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
    quiet.load_patch(2, cook(OPL_PATCH_ORGAN));
    quiet.note_on(0, 2);
    g_state.multi.part[2].volume_q15 = 0;
    Samples q;
    for (uint32_t i = 0; i < 4; i++) quiet.render(q);
    check(silent(q, 0, q.size()), "a Part at volume 0 is silent");
}

void test_a_part_sending_nothing_skips_the_fx() {
    Rig dry;
    dry.load_patch(0, cook(OPL_PATCH_ORGAN));
    dry.note_on(0, 0);
    Samples want;
    for (uint32_t i = 0; i < 6; i++) dry.render(want);

    Rig rig;
    rig.load_patch(0, cook(OPL_PATCH_ORGAN));
    rig.note_on(0, 0);
    g_state.multi.fx.type = FX_DELAY;
    g_state.multi.fx.mix = 127;
    g_state.multi.part[0].send_q15 = 0;
    Samples got;
    for (uint32_t i = 0; i < 6; i++) rig.render(got);
    check(got == want, "a Part with FX SEND 0 plays dry under a full-mix delay");
}

void test_a_note_released_while_waiting_for_its_cook_ends() {
    Rig rig;
    rig.load_patch(0, cook(OPL_PATCH_LEAD));
    rig.note_on(0, 0, 1);  // its cook is still on Core 0
    Samples got;
    rig.render(got);
    rig.note_off(0);       // released before it ever started
    rig.edit_patch(0, cook(OPL_PATCH_LEAD));
    for (uint32_t i = 0; i < 200 && (i == 0 || (g_state.active_mask & 1u)); i++) rig.render(got);
    check(!(g_state.active_mask & 1u), "it starts, releases at once and ends");
}

}  // namespace

int main() {
    opl_init_waveforms();
    osc_init_sine();
    env_dx_init_tables();
    test_every_factory_patch_sounds_as_opl_voice_renders_it();
    test_an_edit_waits_for_the_next_note();
    test_a_new_patch_leaves_a_sounding_note_on_its_own();
    test_a_note_waits_for_its_cook();
    test_a_note_played_just_before_a_program_change_keeps_its_patch();
    test_the_part_mix_pans_and_scales_the_note();
    test_a_part_sending_nothing_skips_the_fx();
    test_a_note_released_while_waiting_for_its_cook_ends();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
