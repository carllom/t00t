// Host test for the chip module on the patch platform, at the platform
// seam: raw MIDI bytes in -- Program Change, NRPN, Part CCs -- through the
// shared dispatch into a Multi of chip Parts, and cooked patches out, read
// from the Core 1 side of each Part's slot. Runs the real chip descriptors,
// cook and factory bank.
//
// Expected instrument fields are the hand-authored instruments.h and
// ay_instruments.h ones, not taken from the cook.

#include "chip_factory.h"
#include "chip_module.h"
#include "instruments.h"
#include "ay_instruments.h"
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

PartSlotT<ChipCookedPatch> chip_part_slots[CHIP_PART_COUNT];
PartSlotT<ChipMultiCooked> chip_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Platform = PatchPlatformT<ChipModule>;
using Multi = MultiT<ChipModule>;

// --- Wiring: what the chip Input subsystem does with these messages ---

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
    UserBank user{ ChipModule::kModuleId, user_flash(flash) };
    Platform platform{ chip_part_slots, kChipFactoryBanks, kChipFactoryBankCount, &user };
    Multi multi{ platform, &user, chip_multi_slot };
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
        ChipCookedPatch c;
        for (auto &s : chip_part_slots) {
            while (s.consume(c)) platform.task();
        }
        ChipMultiCooked m;
        while (chip_multi_slot.consume(m)) multi.task();
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

    // NRPN that lands exactly on `v` for parameter `id`.
    void set(uint8_t channel, uint16_t id, int32_t v) {
        const ParamDescriptor &d = ChipModule::kDescriptors[id];
        nrpn(channel, id, (uint16_t)(((int64_t)(v - d.min) * 16383 + (d.max - d.min) / 2) / (d.max - d.min)));
    }

    // The newest cooked patch Core 1 would take for `part`, if one was published.
    bool take(uint8_t part, ChipCookedPatch &out) {
        bool got = false;
        while (chip_part_slots[part].consume(out)) { got = true; platform.task(); }
        return got;
    }
};

bool same_wave(const ChipWaveTable &c, const WaveTable &t) {
    if (c.len != t.len || c.loop != t.loop) return false;
    for (uint32_t r = 0; r < t.len; r++) {
        const WaveRow &a = c.rows[r], &b = t.rows[r];
        if (a.waveform != b.waveform || a.flags != b.flags || a.note != b.note) return false;
    }
    return true;
}

bool same_sweep(const ChipSweepTable &c, const SweepTable &t) {
    if (c.len != t.len || c.loop != t.loop) return false;
    for (uint32_t r = 0; r < t.len; r++) {
        if (c.rows[r].delta != t.rows[r].delta || c.rows[r].duration != t.rows[r].duration) return false;
    }
    return true;
}

bool plays(const ChipCookedPatch &c, const Instrument &i) {
    return c.type == VT_SID && c.ad == i.ad && c.sr == i.sr && c.vibrato_depth == i.vibrato_depth &&
           c.vibrato_speed == i.vibrato_speed && c.vibrato_delay == i.vibrato_delay &&
           c.gate_off_timer == i.gate_off_timer && c.first_wave == i.first_wave && c.pulse_init == i.pulse_init &&
           c.uses_filter == i.uses_filter && c.filter_cutoff_init == i.filter_cutoff_init &&
           c.filter_resonance == i.filter_resonance && c.filter_mode_mask == i.filter_mode_mask &&
           same_wave(c.wave, i.wave) && same_sweep(c.pulse, i.pulse) && same_sweep(c.filter, i.filter);
}

bool plays(const ChipCookedPatch &c, const AyInstrument &i) {
    if (c.wave.len != i.tone.len || c.wave.loop != i.tone.loop) return false;
    for (uint32_t r = 0; r < i.tone.len; r++) {
        if (c.wave.rows[r].note != i.tone.rows[r].note) return false;
    }
    return c.type == VT_AY && c.ay_model == i.model && c.ay_initial_volume == i.initial_volume &&
           c.ay_use_envelope == i.use_envelope && c.ay_envelope_period == i.envelope_period &&
           c.ay_envelope_shape == i.envelope_shape && c.ay_tone_on == i.tone_on && c.ay_noise_on == i.noise_on &&
           c.ay_noise_period == i.noise_period && same_sweep(c.pulse, i.volume) &&
           c.vibrato_depth == i.vibrato_depth && c.vibrato_speed == i.vibrato_speed &&
           c.vibrato_delay == i.vibrato_delay && c.gate_off_timer == i.gate_off_timer;
}

// --- Tests ---

void test_the_default_multi_plays_the_first_factory_patch() {
    Rig rig;
    const ChipCookedPatch &c = rig.platform.cooked(7);
    check(strcmp(rig.platform.name(7), "ARP LEAD") == 0, "every Part starts on factory bank 0, slot 0");
    check(plays(c, INSTRUMENTS[INS_ARP_LEAD]), "with ARP_LEAD's instrument, its eight-row arpeggio included");
}

void test_program_change_loads_an_ay_instrument() {
    Rig rig;
    rig.midi({ 0xC1, 5 });  // channel 2: program 5, the second AY instrument
    ChipCookedPatch c{};
    check(rig.take(1, c), "Program Change commits a cooked patch to Part 1");
    check(strcmp(rig.platform.name(1), "AY BUZZ BASS") == 0 && plays(c, AY_INSTRUMENTS[AY_INS_BUZZ_BASS]),
          "programs past the SID instruments are AY's: AY BUZZ BASS, envelope-driven");
    check(!rig.take(0, c), "Part 0 (channel 1) is untouched");
}

void test_nrpn_edits_reach_the_table_rows() {
    Rig rig;
    ChipCookedPatch c{};
    rig.set(0, chip_wave_row_note(2), -12);
    check(rig.take(0, c) && c.wave.rows[2].note == -12, "NRPN W3 NOTE = -12");

    rig.set(0, chip_wave_row_wave(2), CHIP_ROW_HOLD | 0x02);
    check(rig.take(0, c) && c.wave.rows[2].waveform == WAVE_HOLD, "W3 WAVE with HOLD set holds the waveform");

    rig.set(0, chip_wave_row_wave(2), 0x04 | CHIP_ROW_SYNC);  // PUL + SYNC
    check(rig.take(0, c) && c.wave.rows[2].waveform == 0x04 && c.wave.rows[2].flags == WAVE_FLAG_SYNC,
          "W3 WAVE = PUL+SYNC: the pulse waveform with the sync toggle");

    rig.set(0, CHIP_P_WAVE_LEN, 24);
    check(rig.take(0, c) && c.wave.len == 24, "the wave table runs to its 24th row");

    rig.set(0, chip_pulse_row_delta(23), -4095);
    rig.set(0, chip_pulse_row_frames(23), 255);
    check(rig.take(0, c) && c.pulse.rows[23].delta == -4095 && c.pulse.rows[23].duration == 255,
          "the last pulse row takes the full range");

    rig.set(0, CHIP_P_FILTER, 0x1 | 0x4);  // LP + HP
    check(rig.take(0, c) && c.uses_filter && c.filter_mode_mask == 0x5, "FILTER = LP+HP: a notch through a bus");
    rig.set(0, CHIP_P_FILTER, 0);
    check(rig.take(0, c) && !c.uses_filter, "FILTER with no bits set plays dry");

    rig.set(0, CHIP_P_ATTACK, 9);
    rig.set(0, CHIP_P_RELEASE, 3);
    check(rig.take(0, c) && c.ad == 0x96 && c.sr == 0xa3, "ATTACK and RELEASE land in the ADSR nibbles");
    check(rig.platform.edited(0) && !rig.take(3, c), "Part 0's Edit buffer is edited; Part 3 got nothing");
}

void test_part_ccs_set_the_mix() {
    Rig rig;
    rig.midi({ 0xB2, 7, 64, 0xB2, 91, 0 });  // channel 3: volume 64, no FX send
    check(rig.multi.part_param(2, PART_P_VOLUME) == 64 && rig.multi.part_param(2, PART_P_SEND) == 0,
          "CC7 and CC91 set the Part's VOLUME and FX SEND");
    ChipCookedPatch c{};
    check(!rig.take(2, c), "and leave its Patch alone");
}

void test_a_stored_table_edit_loads_back_from_the_user_bank() {
    Rig rig;
    rig.midi({ 0xC0, 1 });  // PWM PLUCK
    rig.set(0, chip_pulse_row_delta(1), 100);
    rig.platform.rename(0, "MY PLUCK");
    check(rig.platform.store(0, 5) == StoreResult::OK, "Part 0's Edit buffer stores into user slot 5");
    rig.drain();
    rig.midi({ 0xB1, 0, 64, 0xC1, 5 });  // channel 2: user bank, slot 5
    ChipCookedPatch c{};
    Instrument want = INSTRUMENTS[INS_PWM_PLUCK];
    SweepRow rows[5];
    memcpy(rows, want.pulse.rows, sizeof rows);
    rows[1].delta = 100;
    want.pulse.rows = rows;
    check(rig.take(1, c) && strcmp(rig.platform.name(1), "MY PLUCK") == 0 && plays(c, want),
          "Bank Select 64 + Program Change 5 loads it, edited row and all, on another Part");
}

void test_the_factory_bank_is_the_instruments() {
    const uint32_t count = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;
    bool all = kChipFactoryBankCount == 1 && kChipFactoryBanks[0].count == count;
    for (uint32_t k = 0; all && k < count; k++) {
        int32_t values[CHIP_P_COUNT];
        char name[kPatchNameLen + 1];
        const StoredPatchRef stored = kChipFactoryBanks[0].patches[k];
        ChipCookedPatch c{};
        all = patch_decode(stored.data, stored.len, ModuleId::CHIP, ChipModule::kDescriptors, values, name) ==
              PatchLoadResult::OK;
        if (all) ChipModule::cook(values, c);
        all = all && (k < INSTRUMENT_COUNT ? plays(c, INSTRUMENTS[k]) : plays(c, AY_INSTRUMENTS[k - INSTRUMENT_COUNT]));
    }
    check(all, "the factory bank decodes and cooks to every instrument, SID's then AY's, in program order");
}

void test_a_stored_chip_patch_fits_the_platform() {
    // 21 instrument-wide parameters, 2 bytes for PULSE INIT, CUTOFF and ENV
    // PERIOD; 6 lengths and loops; 24 wave rows of 2 bytes and 48 sweep
    // rows of 3.
    check(CHIP_P_COUNT == 171, "171 parameters");
    check(patch_stored_size(ChipModule::kDescriptors) == kPatchHeaderSize + 24 + 6 + 24 * 2 + 48 * 3,
          "a stored Patch is 246 bytes");
    check(patch_stored_size(ChipModule::kDescriptors) <= kUserPatchSlotSize, "it fits a 256-byte user-bank slot");
    check(sysex_descriptor_dump_fits(ChipModule::kDescriptors), "every descriptor fits a dump message");
    check(QuickEditT<ChipModule>::kQuickCount == 6, "six parameters on the Quick-edit Page");
}

}  // namespace

int main() {
    test_the_default_multi_plays_the_first_factory_patch();
    test_program_change_loads_an_ay_instrument();
    test_nrpn_edits_reach_the_table_rows();
    test_part_ccs_set_the_mix();
    test_a_stored_table_edit_loads_back_from_the_user_bank();
    test_the_factory_bank_is_the_instruments();
    test_a_stored_chip_patch_fits_the_platform();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
