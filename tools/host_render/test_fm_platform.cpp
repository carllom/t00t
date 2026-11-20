// Host test for the FM module on the patch platform, at the platform seam:
// raw MIDI bytes in -- Bank Select, Program Change, NRPN -- through the
// shared dispatch into a Multi of FM Parts, and cooked patches out, read
// from the Core 1 side of each Part's slot. Runs the real FM descriptors
// and cook.
//
// Expected cooked values are worked out by hand from the DX7's algorithm
// diagrams and frequency rules, not by calling the cook.

#include "fm_factory.h"
#include "fm_module.h"
#include "midi/midi_dispatch.h"
#include "midi/midi_parser.h"
#include "patch/multi.h"
#include "patch/platform.h"
#include "patch/stored_format.h"
#include "patch/sysex.h"
#include "ram_flash.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

PartSlotT<FmCookedPatch> fm_part_slots[FM_PART_COUNT];
PartSlotT<FmMultiCooked> fm_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Platform = PatchPlatformT<FmModule>;
using Multi = MultiT<FmModule>;

// --- Test Patches: the INIT VOICE (descriptor defaults) with changes ---

struct Values {
    int32_t v[FM_P_COUNT];
    Values() {
        for (uint32_t i = 0; i < FM_P_COUNT; i++) v[i] = FmModule::kDescriptors[i].def;
    }
    Values &set(uint32_t id, int32_t value) { v[id] = value; return *this; }
    Values &op(uint32_t op, FmOpField f, int32_t value) { return set(fm_op_param(op, f), value); }
};

std::vector<uint8_t> encode(const char *name, const Values &values) {
    std::vector<uint8_t> out(patch_stored_size(FmModule::kDescriptors));
    patch_encode(ModuleId::FM, FmModule::kSchemaVersion, name, FmModule::kDescriptors, values.v,
                 out.data(), (uint32_t)out.size());
    return out;
}

// Bank 0: INIT VOICE. Bank 1: "STACK" -- algorithm 32, feedback 7, OP3 at
// coarse 2 fine 50, OP4 fixed at coarse 1 fine 0 (10 Hz).
const std::vector<uint8_t> kInit = encode("INIT VOICE", Values());
const std::vector<uint8_t> kStack = encode("STACK", Values()
    .set(FM_P_ALGORITHM, 31).set(FM_P_FEEDBACK, 7)
    .op(2, FM_OP_COARSE, 2).op(2, FM_OP_FINE, 50)
    .op(3, FM_OP_MODE, 1).op(3, FM_OP_COARSE, 1).op(3, FM_OP_FINE, 0));
const StoredPatchRef kBank0[] = { { kInit.data(), (uint32_t)kInit.size() } };
const StoredPatchRef kBank1[] = { { kStack.data(), (uint32_t)kStack.size() } };
const FactoryBank kFactory[] = { { kBank0, 1 }, { kBank1, 1 } };

// --- Wiring: what the FM Input subsystem does with these messages ---

struct Wiring {
    Multi *multi;
};

void set_patch(Wiring &w, const InputValue &value) {
    w.multi->program_change(value.channel, midi_channel_bank_msb(value.channel), (uint8_t)value.index);
}

void set_param_cc(Wiring &w, const InputValue &value) {
    w.multi->control_change(value.channel, value.id, (uint8_t)value.scalar);
}

constexpr InputMapEntryT<Wiring> kTable[] = {
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0, set_patch },
    { InputCategory::MODIFIER, 0, 127, 0xFF, 0, set_param_cc },
};

struct Rig {
    RamFlash flash;
    UserBank user{ FmModule::kModuleId, user_flash(flash) };
    Platform platform{ fm_part_slots, kFactory, 2, &user };
    Multi multi{ platform, &user, fm_multi_slot };
    Wiring wiring{ &multi };
    MidiParser parser{};

    Rig() {
        parser.init();
        midi_bank_select_init();
        multi.init();
        drain();
    }

    // Core 1's side: drop every publish so far.
    void drain() {
        FmCookedPatch c;
        for (auto &s : fm_part_slots) {
            while (s.consume(c)) platform.task();
        }
        FmMultiCooked m;
        while (fm_multi_slot.consume(m)) multi.task();
    }

    void midi(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            MidiEvent ev;
            if (!parser.feed(b, ev)) continue;
            if (ev.type == MIDI_PROGRAM_CHANGE) midi_dispatch_program_change(wiring, kTable, ev.channel, ev.data1);
            else if (ev.type == MIDI_CC && ev.data1 == 0) midi_bank_select_msb(ev.channel, ev.data2);
            else if (ev.type == MIDI_CC) midi_dispatch_cc(wiring, kTable, ev.channel, ev.data1, ev.data2);
        }
    }

    // NRPN `id` = 14-bit `value14` on `channel` (0-based).
    void nrpn(uint8_t channel, uint16_t id, uint16_t value14) {
        const uint8_t cc = (uint8_t)(0xB0 | channel);
        midi({ cc, 99, (uint8_t)(id >> 7), cc, 98, (uint8_t)(id & 0x7F),
               cc, 6, (uint8_t)(value14 >> 7), cc, 38, (uint8_t)(value14 & 0x7F) });
    }

    // The newest cooked patch Core 1 would take for `part`, if one was published.
    bool take(uint8_t part, FmCookedPatch &out) {
        bool got = false;
        while (fm_part_slots[part].consume(out)) { got = true; platform.task(); }
        return got;
    }
};

// The 14-bit NRPN value that lands exactly on `v` in `d`'s range.
uint16_t value14(const ParamDescriptor &d, int32_t v) {
    return (uint16_t)(((int64_t)(v - d.min) * 16383 + (d.max - d.min) / 2) / (d.max - d.min));
}

bool routing_is(const FmRouting &r, uint8_t num_ops, std::initializer_list<uint8_t> order,
                std::initializer_list<uint8_t> in_bus, std::initializer_list<uint8_t> out_bus,
                std::initializer_list<uint8_t> kernel) {
    if (!r.valid || r.num_ops != num_ops) return false;
    uint32_t i = 0;
    for (uint8_t o : order) if (r.order[i++] != o) return false;
    i = 0;
    for (uint8_t b : in_bus) if (r.in_bus[i++] != b) return false;
    i = 0;
    for (uint8_t b : out_bus) if (r.out_bus[i++] != b) return false;
    i = 0;
    for (uint8_t k : kernel) {
        if (r.out_bus[i] != FM_BUS_ZERO && r.kernel[i] != k) return false;
        i++;
    }
    return true;
}

constexpr uint8_t OUT = FM_TARGET_OUT, Z = FM_BUS_ZERO;
constexpr uint8_t F = FM_KERNEL_FIRST, P = FM_KERNEL_PLAIN, FB = FM_KERNEL_FEEDBACK;

// --- Tests ---

void test_the_default_multi_plays_the_init_voice() {
    Rig rig;
    const FmCookedPatch &c = rig.platform.cooked(0);
    check(strcmp(rig.platform.name(5), "INIT VOICE") == 0, "every Part starts on factory bank 0, slot 0");
    // Algorithm 1: OP2 -> OP1, OP6 -> OP5 -> OP4 -> OP3; OP1 and OP3 are
    // the carriers.
    check(routing_is(c.routing, 6, { 1, 0, 5, 4, 3, 2 }, { 0, Z, 2, 3, 4, Z }, { OUT, 0, OUT, 2, 3, 4 },
                     { F, F, P, F, F, F }),
          "algorithm 1, resolved on Core 0: render order, buses and kernels");
    check(c.patch.op[0].output_level == 99 && c.patch.op[1].output_level == 0 && c.patch.op[0].ratio == 1.0f &&
          !c.patch.op[0].fixed_freq && c.patch.op[0].eg_level[3] == 0 && c.patch.op[0].scale_breakpoint == 39,
          "OP1 alone at level 99, ratio 1, L4 0, break point C3");
    check(c.patch.lfo.rate == 35 && c.patch.lfo.key_sync && c.patch.lfo.pms == 3 &&
          c.patch.pitch_eg.level[0] == 50 && c.patch.pitch_eg.level[3] == 50,
          "LFO speed 35, key sync, PMS 3; pitch EG levels at 50 (no deviation)");
}

void test_bank_select_and_program_change_load_another_bank() {
    Rig rig;
    rig.midi({ 0xB0, 0, 1, 0xC0, 0 });  // channel 1: bank 1, program 0
    FmCookedPatch c{};
    check(rig.take(0, c), "Program Change commits a cooked patch to Part 0");
    check(strcmp(rig.platform.name(0), "STACK") == 0, "loaded from the bank Bank Select picked");
    // Algorithm 32: six carriers, OP6 feeding back on itself.
    check(routing_is(c.routing, 6, { 0, 1, 2, 3, 4, 5 }, { Z, Z, Z, Z, Z, Z }, { OUT, OUT, OUT, OUT, OUT, OUT },
                     { F, P, P, P, P, FB }) &&
          c.routing.fb_shift[5] == 1 && c.patch.op[5].feedback_level == 7 && c.patch.op[0].feedback_level == 0,
          "algorithm 32: all carriers, OP6 feedback at level 7 (shift 1)");
    check(c.patch.op[2].ratio == 3.0f, "OP3 coarse 2, fine 50: ratio 3.0");
    check(c.patch.op[3].fixed_freq && c.patch.op[3].fixed_hz == 10.0f && c.patch.op[3].ratio == 1.0f,
          "OP4 fixed, coarse 1 fine 0: 10 Hz");
    check(!rig.take(1, c), "Part 1 (channel 2) is untouched");
}

void test_nrpn_edits_reach_the_cooked_patch() {
    Rig rig;
    FmCookedPatch c{};
    const uint16_t level = fm_op_param(0, FM_OP_LEVEL);
    rig.nrpn(0, level, value14(FmModule::kDescriptors[level], 50));
    check(rig.take(0, c) && c.patch.op[0].output_level == 50, "NRPN OP1 LEVEL = 50 is cooked and published");

    rig.nrpn(0, FM_P_ALGORITHM, 16383);
    check(rig.take(0, c) && c.routing.out_bus[5] == OUT && c.routing.kernel[0] == F,
          "NRPN ALGORITHM at full scale: algorithm 32, every operator a carrier");

    const uint16_t coarse = fm_op_param(1, FM_OP_COARSE);
    rig.nrpn(0, coarse, value14(FmModule::kDescriptors[coarse], 0));
    check(rig.take(0, c) && c.patch.op[1].ratio == 0.5f, "NRPN OP2 COARSE = 0: ratio 0.5 (the DX7's coarse 0)");

    rig.nrpn(0, FM_P_LFO_PMD, value14(FmModule::kDescriptors[FM_P_LFO_PMD], 40));
    check(rig.take(0, c) && c.patch.lfo.pmd == 40, "NRPN LFO PMD = 40");
    check(rig.platform.edited(0) && !rig.take(3, c), "Part 0's Edit buffer is edited; Part 3 got nothing");
}

void test_a_disabled_operator_leaves_the_render_order() {
    Rig rig;
    FmCookedPatch c{};
    rig.nrpn(0, fm_op_param(1, FM_OP_ENABLE), 0);  // OP2 off: OP1 loses its modulator
    check(rig.take(0, c), "OP2 ENABLE = OFF is cooked and published");
    check(routing_is(c.routing, 5, { 0, 5, 4, 3, 2 }, { Z, Z, 2, 3, 4, Z }, { OUT, Z, OUT, 2, 3, 4 },
                     { F, 0, P, F, F, F }),
          "five operators rendered; OP1 reads the zero bus; OP2 writes nothing");

    rig.nrpn(0, fm_op_param(0, FM_OP_ENABLE), 0);
    rig.nrpn(0, fm_op_param(2, FM_OP_ENABLE), 0);
    check(rig.take(0, c) && c.routing.num_ops == 3 && c.routing.out_bus[0] == Z && c.routing.out_bus[2] == Z,
          "with both carriers off, no operator reaches the output");
}

void test_layered_parts_load_together() {
    Rig rig;
    rig.multi.set_part(1, PART_P_CHANNEL, 0);  // Part 1 onto channel 1 with Part 0
    rig.drain();
    rig.midi({ 0xB0, 0, 1, 0xC0, 0 });
    FmCookedPatch a{}, b{};
    check(rig.take(0, a) && rig.take(1, b) && strcmp(rig.platform.name(1), "STACK") == 0 &&
          memcmp(&a.routing, &b.routing, sizeof a.routing) == 0,
          "a Program Change loads STACK into both Parts on the channel");
}

void test_a_stored_fm_patch_fits_the_platform() {
    check(patch_stored_size(FmModule::kDescriptors) == kPatchHeaderSize + FM_P_COUNT,
          "151 parameters, one byte each, after the 24-byte header");
    check(sysex_descriptor_dump_fits(FmModule::kDescriptors), "every descriptor fits a dump message");
}

void test_the_built_in_bank_holds_the_init_voice() {
    int32_t values[FM_P_COUNT];
    char name[kPatchNameLen + 1];
    const StoredPatchRef init = kFmFactoryBanks[0].patches[0];
    bool defaults = kFmFactoryBankCount == 1 && kFmFactoryBanks[0].count == 1 &&
                    patch_decode(init.data, init.len, ModuleId::FM, FmModule::kDescriptors, values, name) ==
                        PatchLoadResult::OK;
    for (uint32_t i = 0; defaults && i < FM_P_COUNT; i++) defaults = values[i] == FmModule::kDescriptors[i].def;
    check(defaults && strcmp(name, "INIT VOICE") == 0,
          "without generated banks, one built-in bank holds the INIT VOICE: every parameter at its default");
}

}  // namespace

int main() {
    test_the_default_multi_plays_the_init_voice();
    test_bank_select_and_program_change_load_another_bank();
    test_nrpn_edits_reach_the_cooked_patch();
    test_a_disabled_operator_leaves_the_render_order();
    test_layered_parts_load_together();
    test_a_stored_fm_patch_fits_the_platform();
    test_the_built_in_bank_holds_the_init_voice();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
