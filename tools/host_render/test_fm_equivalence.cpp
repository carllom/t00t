// Render equivalence for the FM factory banks: every factory Patch played
// through the patch platform -- stored format, decode, cook, Part slot,
// Core 1's render (fm_render.h) -- against op.h rendering the converter's
// FmPatch for the same DX7 voice (patches.h) directly.
//
//   test_fm_equivalence [first bank]
//
// Both inputs are generated locally from the same .syx files and are
// gitignored: factory_banks.h by `syx2patch.py bank`, patches.h by
// `syx2patch.py convert` (at most four files). patches.h's Patches are
// matched with factory banks from `first bank` (default 0) on. Without
// either header the test reports that and passes.

#include "fm_factory.h"
#include "fm_render.h"
#include "patch/stored_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

PartSlotT<FmCookedPatch> fm_part_slots[FM_PART_COUNT];
PartSlotT<FmMultiCooked> fm_multi_slot;

#if defined(T00T_FM_HAS_PATCHES) && defined(T00T_FM_HAS_FACTORY_BANKS)

namespace {

constexpr uint32_t kFrames = SAMPLES_PER_BUFFER;
constexpr uint32_t kHeld = 200;     // buffers, ~1.2 s
constexpr uint32_t kBuffers = 300;  // then ~0.6 s of release
constexpr uint8_t kNote = 60;
constexpr int16_t kAmplitude = 100 * 258;

bool same_op(const FmOpParams &a, const FmOpParams &b) {
    return a.ratio == b.ratio && a.fixed_hz == b.fixed_hz && a.fixed_freq == b.fixed_freq &&
           a.detune_offset == b.detune_offset && a.mod_target == b.mod_target &&
           a.extra_target_mask == b.extra_target_mask && a.feedback_level == b.feedback_level &&
           a.output_level == b.output_level && a.vel_sensitivity == b.vel_sensitivity &&
           memcmp(a.eg_rate, b.eg_rate, 4) == 0 && memcmp(a.eg_level, b.eg_level, 4) == 0 &&
           a.scale_breakpoint == b.scale_breakpoint && a.scale_left_depth == b.scale_left_depth &&
           a.scale_right_depth == b.scale_right_depth && a.scale_left_curve == b.scale_left_curve &&
           a.scale_right_curve == b.scale_right_curve && a.rate_scaling == b.rate_scaling &&
           a.am_sensitivity == b.am_sensitivity;
}

bool same_patch(const FmPatch &a, const FmPatch &b) {
    for (uint32_t i = 0; i < FM_NUM_OPS; i++) if (!same_op(a.op[i], b.op[i])) return false;
    return memcmp(&a.lfo, &b.lfo, sizeof a.lfo) == 0 && memcmp(&a.pitch_eg, &b.pitch_eg, sizeof a.pitch_eg) == 0;
}

// The reference: op.h on the converter's const-table FmPatch, its routing
// resolved here.
std::vector<int16_t> render_old(const FmPatch &patch, uint32_t note_inc) {
    FmOp ops[FM_NUM_OPS];
    FmPitchEg peg;
    FmLfo lfo;
    FmRouting routing;
    static int32_t bus_mod[FM_NUM_OPS][FM_BLOCK], bus_out[FM_BLOCK];
    for (auto &op : ops) env_dx_reset(op.eg);
    fm_pitch_eg_init(peg);
    fm_lfo_init(lfo);
    std::vector<int16_t> out;
    if (!fm_resolve_routing(patch, routing)) return out;
    fm_voice_note_on<DxEnvGlue>(ops, patch, note_inc, kAmplitude, kNote, &peg, &lfo);
    FmVoiceBuses bus{ { bus_mod[0], bus_mod[1], bus_mod[2], bus_mod[3], bus_mod[4], bus_mod[5] }, bus_out };
    for (uint32_t b = 0; b < kBuffers; b++) {
        if (b == kHeld) fm_voice_note_off<DxEnvGlue>(ops, &peg);
        int32_t l[kFrames] = {}, r[kFrames] = {};
        if (b < kHeld || fm_voice_active(ops, routing)) {
            fm_render_voice<FM_TABLE_BITS, DxEnvGlue>(ops, patch, routing, bus, 0, l, r, kFrames, &peg, &lfo,
                                                       note_inc, 0);
        }
        for (uint32_t i = 0; i < kFrames; i++) {
            out.push_back((int16_t)__ssat(l[i], 16));
            out.push_back((int16_t)__ssat(r[i], 16));
        }
    }
    return out;
}

FmRenderState g_state;

// The platform path, from the stored Patch.
std::vector<int16_t> render_new(const StoredPatchRef &stored, uint32_t note_inc, FmCookedPatch &cooked) {
    int32_t values[FM_P_COUNT];
    char name[kPatchNameLen + 1];
    std::vector<int16_t> out;
    if (patch_decode(stored.data, stored.len, ModuleId::FM, FmModule::kDescriptors, values, name) !=
        PatchLoadResult::OK) return out;
    FmModule::cook(values, cooked);

    fm_render_init(g_state);
    g_state.multi.fx.type = FX_OFF;
    fm_part_slots[0].try_publish(cooked, 1, 1);
    VoiceParamBlock vp{};
    for (auto &v : vp.voices) v = voice_params_default<VoiceParams>();
    VoiceParams &p = vp.voices[0];
    p = { note_inc, kAmplitude, 1, true, 0, kNote, 1, 1, 0 };
    for (uint32_t b = 0; b < kBuffers; b++) {
        if (b == kHeld) p.gate = false;
        fm_render_take_patch(g_state, 0, fm_part_slots[0]);
        int16_t buf[kFrames * 2];
        fm_render_buffer(g_state, vp, buf);
        out.insert(out.end(), buf, buf + kFrames * 2);
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();
    const uint32_t first_bank = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
    const uint32_t note_inc = fm_phase_inc(261.6256f);

    uint32_t compared = 0, failures = 0, silent = 0;
    for (uint32_t k = 0; k < FM_PATCH_COUNT; k++) {
        const uint32_t bank = first_bank + k / 32, slot = k % 32;
        if (bank >= kFmFactoryBankCount || slot >= kFmFactoryBanks[bank].count) break;
        const StoredPatchRef stored = kFmFactoryBanks[bank].patches[slot];
        char name[kPatchNameLen + 1];
        patch_read_name(stored.data, name);
        const FmPatch &old = FM_PATCHES[k];
        const bool same_name = strcmp(name, old.name) == 0;
        FmCookedPatch cooked{};
        const std::vector<int16_t> got = render_new(stored, note_inc, cooked);
        FmRouting old_routing{};
        fm_resolve_routing(old, old_routing);
        const bool fields = same_patch(cooked.patch, old);
        const bool routing = memcmp(cooked.routing.order, old_routing.order, FM_NUM_OPS) == 0 &&
                             memcmp(cooked.routing.in_bus, old_routing.in_bus, FM_NUM_OPS) == 0 &&
                             memcmp(cooked.routing.out_bus, old_routing.out_bus, FM_NUM_OPS) == 0 &&
                             memcmp(cooked.routing.kernel, old_routing.kernel, FM_NUM_OPS) == 0 &&
                             cooked.routing.clear_bus_mask == old_routing.clear_bus_mask;
        const bool audio = !got.empty() && got == render_old(old, note_inc);
        bool any = false;
        for (int16_t x : got) any = any || x != 0;
        silent += any ? 0 : 1;
        compared++;
        if (!same_name || !fields || !routing || !audio) {
            failures++;
            printf("FAIL bank %u slot %u \"%s\" vs \"%s\":%s%s%s%s\n", bank, slot, name, old.name,
                   same_name ? "" : " name", fields ? "" : " fields", routing ? "" : " routing",
                   audio ? "" : " audio");
        }
    }
    printf("%u factory Patches compared, %u differ, %u silent\n", compared, failures, silent);
    const bool ok = failures == 0 && compared > 0 && silent == 0;
    printf(ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return ok ? 0 : 1;
}

#else

int main() {
    printf("skipped: needs factory_banks.h and patches.h, generated locally by tools/syx2patch.py\n");
    printf("ALL CHECKS PASSED\n");
    return 0;
}

#endif
