// Host test for the OPL module on the patch platform, at the platform seam:
// raw MIDI bytes in -- Program Change, NRPN, Part CCs -- through the shared
// dispatch into a Multi of OPL Parts, and cooked patches out, read from the
// Core 1 side of each Part's slot. Runs the real OPL descriptors, cook and
// factory bank.
//
// Expected routings are worked out by hand from the Algorithms' connection
// diagrams, and expected register values are patches.h's, not taken from
// the cook.

#include "opl_factory.h"
#include "opl_module.h"
#include "patches.h"
#include "midi/midi_dispatch.h"
#include "midi/midi_parser.h"
#include "patch/multi.h"
#include "patch/platform.h"
#include "patch/quick_edit.h"
#include "patch/stored_format.h"
#include "patch/sysex.h"
#include "ram_flash.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

PartSlotT<OplCookedPatch> opl_part_slots[OPL_PART_COUNT];
PartSlotT<OplMultiCooked> opl_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Platform = PatchPlatformT<OplModule>;
using Multi = MultiT<OplModule>;

// --- Wiring: what the OPL Input subsystem does with these messages ---

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
    UserBank user{ OplModule::kModuleId, user_flash(flash) };
    Platform platform{ opl_part_slots, kOplFactoryBanks, kOplFactoryBankCount, &user };
    Multi multi{ platform, &user, opl_multi_slot };
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
        OplCookedPatch c;
        for (auto &s : opl_part_slots) {
            while (s.consume(c)) platform.task();
        }
        OplMultiCooked m;
        while (opl_multi_slot.consume(m)) multi.task();
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
    bool take(uint8_t part, OplCookedPatch &out) {
        bool got = false;
        while (opl_part_slots[part].consume(out)) { got = true; platform.task(); }
        return got;
    }
};

// The 14-bit NRPN value that lands exactly on `v` in `d`'s range.
uint16_t value14(const ParamDescriptor &d, int32_t v) {
    return (uint16_t)(((int64_t)(v - d.min) * 16383 + (d.max - d.min) / 2) / (d.max - d.min));
}

bool routing_is(const FmRouting &r, uint8_t num_ops, std::initializer_list<uint8_t> in_bus,
                std::initializer_list<uint8_t> out_bus, std::initializer_list<uint8_t> kernel) {
    if (!r.valid || r.num_ops != num_ops) return false;
    for (uint32_t i = 0; i < num_ops; i++) {
        if (r.order[i] != i) return false;
    }
    uint32_t i = 0;
    for (uint8_t b : in_bus) if (r.in_bus[i++] != b) return false;
    i = 0;
    for (uint8_t b : out_bus) if (r.out_bus[i++] != b) return false;
    i = 0;
    for (uint8_t k : kernel) if (r.kernel[i++] != k) return false;
    return true;
}

bool same_op(const OplOpParams &a, const OplOpParams &b) {
    return a.mult == b.mult && a.ksl == b.ksl && a.tl == b.tl && a.ar == b.ar && a.dr == b.dr &&
           a.sl == b.sl && a.rr == b.rr && a.egt == b.egt && a.ksr == b.ksr && a.ws == b.ws;
}

bool plays(const OplCookedPatch &c, const OplPatch &p) {
    for (uint32_t i = 0; i < OPL_NUM_OPS; i++) {
        if (!same_op(c.patch.op[i], p.op[i])) return false;
    }
    return c.patch.feedback == p.feedback && c.patch.algorithm == p.algorithm;
}

constexpr uint8_t OUT = FM_TARGET_OUT, Z = FM_BUS_ZERO;
constexpr uint8_t F = FM_KERNEL_FIRST, P = FM_KERNEL_PLAIN, FB = FM_KERNEL_FEEDBACK;

// --- Tests ---

void test_the_default_multi_plays_the_first_factory_patch() {
    Rig rig;
    const OplCookedPatch &c = rig.platform.cooked(7);
    check(strcmp(rig.platform.name(7), "OPL LEAD") == 0, "every Part starts on factory bank 0, slot 0");
    check(plays(c, OPL_PATCH_LEAD), "with OPL LEAD's registers");
    // 1>2 with feedback 3: OP1 feeds back on itself (shift 8 - 3) into bus
    // 1, which it clears; OP2 reads bus 1 and carries.
    check(routing_is(c.routing, 2, { Z, 1 }, { 1, OUT }, { FB, F }) && c.routing.fb_shift[0] == 5 &&
          c.routing.clear_bus_mask == (1u << 1),
          "1>2, resolved on Core 0 with OP1's feedback patched in");
}

void test_program_change_loads_a_4op_patch() {
    Rig rig;
    rig.midi({ 0xC1, 5 });  // channel 2: program 5
    OplCookedPatch c{};
    check(rig.take(1, c), "Program Change commits a cooked patch to Part 1");
    check(strcmp(rig.platform.name(1), "OPL4 CHAIN EP") == 0 && plays(c, OPL4_PATCH_CHAIN_EP),
          "slot 5 of the factory bank: OPL4 CHAIN EP");
    // 1>2>3>4: each operator modulates the next; OP4 carries.
    check(routing_is(c.routing, 4, { Z, 1, 2, 3 }, { 1, 2, 3, OUT },
                     { OPL4_PATCH_CHAIN_EP.feedback ? FB : F, F, F, F }),
          "the full 4-operator chain");
    check(!rig.take(0, c), "Part 0 (channel 1) is untouched");
}

void test_an_empty_or_missing_bank_is_ignored() {
    Rig rig;
    OplCookedPatch c{};
    rig.midi({ 0xB0, 0, 1, 0xC0, 3 });  // factory bank 1 doesn't exist
    check(!rig.take(0, c) && strcmp(rig.platform.name(0), "OPL LEAD") == 0, "a missing factory bank leaves the Part");
    rig.midi({ 0xB0, 0, 0, 0xC0, 9 });  // past the factory bank's end
    check(!rig.take(0, c), "so does a program past the bank's end");
    rig.midi({ 0xB0, 0, 64, 0xC0, 0 });  // an empty user slot
    check(!rig.take(0, c), "and an empty user slot");
}

void test_nrpn_edits_reach_the_cooked_patch() {
    Rig rig;
    OplCookedPatch c{};
    const uint16_t tl = opl_op_param(0, OPL_OP_TL);
    rig.nrpn(0, tl, value14(OplModule::kDescriptors[tl], 40));
    check(rig.take(0, c) && c.patch.op[0].tl == 40, "NRPN OP1 TL = 40 is cooked and published");

    rig.nrpn(0, OPL_P_ALGORITHM, 16383);
    // 1+2>3+4: OP1 carries, OP2 modulates OP3, which carries, and OP4
    // carries; OP1 keeps its feedback.
    check(rig.take(0, c) && routing_is(c.routing, 4, { Z, Z, 2, Z }, { OUT, 2, OUT, OUT }, { FB, F, P, P }),
          "NRPN ALGORITHM at full scale: 1+2>3+4");

    rig.nrpn(0, OPL_P_FEEDBACK, 0);
    check(rig.take(0, c) && c.routing.kernel[0] == F && c.routing.clear_bus_mask == 0,
          "NRPN FEEDBACK = 0: OP1 back to a plain first operator");

    const uint16_t wave = opl_op_param(3, OPL_OP_WAVE);
    rig.nrpn(0, wave, value14(OplModule::kDescriptors[wave], 7));
    check(rig.take(0, c) && c.patch.op[3].ws == 7, "NRPN OP4 WAVE = LOG SAW");

    const uint16_t egt = opl_op_param(1, OPL_OP_EGT);
    rig.nrpn(0, egt, 0);
    check(rig.take(0, c) && !c.patch.op[1].egt, "NRPN OP2 EG TYPE = PERC");
    check(rig.platform.edited(0) && !rig.take(3, c), "Part 0's Edit buffer is edited; Part 3 got nothing");
}

void test_part_ccs_set_the_mix() {
    Rig rig;
    rig.midi({ 0xB2, 10, 0, 0xB2, 7, 64 });  // channel 3: pan hard left, volume 64
    check(rig.multi.part_param(2, PART_P_PAN) == 0 && rig.multi.part_param(2, PART_P_VOLUME) == 64,
          "CC10 and CC7 set the Part's PAN and VOLUME");
    OplCookedPatch c{};
    check(!rig.take(2, c), "and leave its Patch alone");
}

void test_a_stored_edit_loads_back_from_the_user_bank() {
    Rig rig;
    rig.midi({ 0xC0, 6 });  // OPL4 DUAL SAW
    rig.nrpn(0, opl_op_param(2, OPL_OP_MULT), value14(OplModule::kDescriptors[opl_op_param(2, OPL_OP_MULT)], 3));
    rig.platform.rename(0, "MY SAW");
    check(rig.platform.store(0, 5) == StoreResult::OK, "Part 0's Edit buffer stores into user slot 5");
    rig.drain();
    rig.midi({ 0xB1, 0, 64, 0xC1, 5 });  // channel 2: user bank, slot 5
    OplCookedPatch c{};
    OplPatch want = OPL4_PATCH_DUAL_SAW;
    want.op[2].mult = 3;
    check(rig.take(1, c) && strcmp(rig.platform.name(1), "MY SAW") == 0 && plays(c, want),
          "Bank Select 64 + Program Change 5 loads it on another Part");
}

void test_the_factory_bank_is_patches_h() {
    bool all = kOplFactoryBankCount == 1 && kOplFactoryBanks[0].count == OPL_PATCH_COUNT;
    for (uint32_t k = 0; all && k < OPL_PATCH_COUNT; k++) {
        int32_t values[OPL_P_COUNT];
        char name[kPatchNameLen + 1];
        const StoredPatchRef stored = kOplFactoryBanks[0].patches[k];
        OplCookedPatch c{};
        all = patch_decode(stored.data, stored.len, ModuleId::OPL, OplModule::kDescriptors, values, name) ==
                  PatchLoadResult::OK &&
              strcmp(name, OPL_PATCHES[k]->name) == 0;
        if (all) OplModule::cook(values, c);
        all = all && plays(c, *OPL_PATCHES[k]);
    }
    check(all, "the factory bank decodes and cooks to every patches.h patch, in order, by name");
}

void test_a_stored_opl_patch_fits_the_platform() {
    check(patch_stored_size(OplModule::kDescriptors) == kPatchHeaderSize + OPL_P_COUNT,
          "42 parameters, one byte each, after the 24-byte header");
    check(sysex_descriptor_dump_fits(OplModule::kDescriptors), "every descriptor fits a dump message");
    check(QuickEditT<OplModule>::kQuickCount == 6, "six parameters on the Quick-edit Page");
}

}  // namespace

int main() {
    test_the_default_multi_plays_the_first_factory_patch();
    test_program_change_loads_a_4op_patch();
    test_an_empty_or_missing_bank_is_ignored();
    test_nrpn_edits_reach_the_cooked_patch();
    test_part_ccs_set_the_mix();
    test_a_stored_edit_loads_back_from_the_user_bank();
    test_the_factory_bank_is_patches_h();
    test_a_stored_opl_patch_fits_the_platform();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
