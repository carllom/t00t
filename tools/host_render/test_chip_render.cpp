// Host test for the chip engine's Core 1 render (src/engines/chip/
// chip_render.h) at its seam: cooked patches published to Part slots and a
// voice block in, interleaved audio out. Each check compares two runs of
// the engine that differ only in what is under test; render_chip covers
// equivalence with the engine before the patch platform.

#include "chip_factory.h"
#include "chip_module.h"
#include "chip_render.h"
#include "patch/stored_format.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Samples = std::vector<int16_t>;
constexpr uint32_t kFrames = SAMPLES_PER_BUFFER;

// Factory program numbers (instruments.h's SID instruments, then AY's).
constexpr uint32_t kArpLead = 0, kPwmPluck = 1, kFilterPad = 2, kAyLead = 4;

// Factory Patch `k`'s values, to edit before cooking.
struct Values {
    int32_t v[CHIP_P_COUNT];
    explicit Values(uint32_t k) {
        const StoredPatchRef s = kChipFactoryBanks[0].patches[k];
        char name[kPatchNameLen + 1];
        if (patch_decode(s.data, s.len, ModuleId::CHIP, ChipModule::kDescriptors, v, name) != PatchLoadResult::OK) {
            fprintf(stderr, "factory patch %u doesn't decode\n", k);
            exit(2);
        }
    }
    ChipCookedPatch cook() const {
        ChipCookedPatch c{};
        ChipModule::cook(v, c);
        return c;
    }
};

ChipCookedPatch factory(uint32_t k) { return Values(k).cook(); }

// --- The engine under test, with the Multi's FX off; each Rig has its own
// Part slots, so two can run side by side ---

struct Rig {
    std::unique_ptr<ChipRenderState> state = std::make_unique<ChipRenderState>();
    std::unique_ptr<PartSlotT<ChipCookedPatch>[]> slots = std::make_unique<PartSlotT<ChipCookedPatch>[]>(CHIP_PART_COUNT);
    VoiceParamBlock vp{};
    uint16_t load[CHIP_PART_COUNT] = {};
    uint16_t seq[CHIP_PART_COUNT] = {};
    VoiceType type[CHIP_PART_COUNT] = {};
    bool filters[CHIP_PART_COUNT] = {};

    Rig() {
        for (auto &v : vp.voices) v = voice_params_default<VoiceParams>();
        chip_render_init(*state);
        state->multi.fx.type = FX_OFF;
    }

    // Core 0's side: a new Patch for `part`, or an edit of the one it has.
    void load_patch(uint8_t part, const ChipCookedPatch &c) { publish(part, c, ++load[part]); }
    void edit_patch(uint8_t part, const ChipCookedPatch &c) { publish(part, c, load[part]); }
    void publish(uint8_t part, const ChipCookedPatch &c, uint16_t load_id) {
        if (!slots[part].try_publish(c, load_id, ++seq[part])) check(false, "the Part slot accepts the publish");
        type[part] = c.type;
        filters[part] = c.uses_filter;
    }

    // Note `note` on `part` with the Part's latest cook (seq_ahead: a cook
    // Core 0 has made but not yet published), its filter bus bus 0.
    void note_on(uint8_t v, uint8_t part, uint8_t note = 60, uint16_t seq_ahead = 0) {
        VoiceParams &p = vp.voices[v];
        const float hz = chip_note_to_hz(note);
        p.type = type[part];
        p.sid_freq = chip_hz_to_freq_reg(hz);
        p.ay_period = ay_hz_to_period(hz);
        p.part = part;
        p.load_id = load[part];
        p.cook_seq = (uint16_t)(seq[part] + seq_ahead);
        p.velocity = 100;
        p.filter_bus = (type[part] == VT_SID && filters[part]) ? 0 : BUS_NONE;
        p.trigger++;
        p.gate = true;
    }
    void note_off(uint8_t v) { vp.voices[v].gate = false; }

    void render(Samples &out, uint32_t buffers = 1) {
        for (uint32_t b = 0; b < buffers; b++) {
            for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) chip_render_take_patch(*state, p, slots[p]);
            int16_t buf[kFrames * 2];
            chip_render_buffer(*state, vp, buf);
            out.insert(out.end(), buf, buf + kFrames * 2);
        }
    }
};

bool silent(const Samples &s) {
    for (int16_t x : s) if (x) return false;
    return true;
}

int32_t peak(const Samples &s) {
    int32_t m = 0;
    for (int16_t x : s) m = abs(x) > m ? abs(x) : m;
    return m;
}

// --- Tests ---

void test_an_edit_waits_for_the_next_note() {
    Values noisy(kArpLead);
    noisy.v[CHIP_P_FIRST_WAVE] = 0x8;  // NOI
    for (uint32_t r = 0; r < CHIP_TABLE_ROWS; r++) noisy.v[chip_wave_row_wave(r)] = 0x8;

    Samples got, want;
    Rig edited, plain;
    for (Rig *r : { &edited, &plain }) {
        r->load_patch(0, factory(kArpLead));
        r->note_on(0, 0);
    }
    for (uint32_t b = 0; b < 8; b++) {
        if (b == 3) edited.edit_patch(0, noisy.cook());
        edited.render(got);
        plain.render(want);
    }
    check(got == want && !silent(got), "a wave-table edit leaves the sounding note as it started");

    edited.note_on(1, 0, 67);
    plain.note_on(1, 0, 67);
    Samples next, next_plain;
    edited.render(next, 4);
    plain.render(next_plain, 4);
    check(next != next_plain, "the next note plays the edited table");
}

void test_a_new_patch_leaves_a_sounding_note_on_its_own() {
    Samples got, want;
    Rig changed, plain;
    for (Rig *r : { &changed, &plain }) {
        r->load_patch(0, factory(kPwmPluck));
        r->note_on(0, 0);
    }
    for (uint32_t b = 0; b < 10; b++) {
        if (b == 3) changed.load_patch(0, factory(kAyLead));
        changed.render(got);
        plain.render(want);
    }
    check(got == want && !silent(got), "a Program Change to an AY Patch leaves the held SID note on its Patch");
}

void test_a_note_waits_for_its_cook() {
    Rig waiting;
    waiting.load_patch(0, factory(kArpLead));
    waiting.note_on(0, 0, 60, 1);  // played after an edit whose publish is still pending
    Samples got;
    waiting.render(got);
    check(silent(got), "the note waits while its cook is still on Core 0");
    waiting.edit_patch(0, factory(kPwmPluck));
    waiting.render(got, 6);

    // The same note played a buffer later on a Part that already has that cook.
    Rig later;
    later.load_patch(0, factory(kPwmPluck));
    Samples want;
    later.render(want);
    later.note_on(0, 0);
    later.render(want, 6);
    check(got == want && !silent(got), "and starts a buffer later on that cook");
}

void test_a_note_played_just_before_a_program_change_keeps_its_patch() {
    Samples got, want;
    Rig changed, plain;
    for (Rig *r : { &changed, &plain }) {
        r->load_patch(0, factory(kArpLead));
        Samples first;
        r->render(first);
        r->note_on(0, 0);
    }
    // Core 0: the note, then a Program Change, both before Core 1's next buffer.
    changed.load_patch(0, factory(kAyLead));
    changed.render(got, 6);
    plain.render(want, 6);
    check(got == want && !silent(got), "it starts on the Part's previous cook, the Patch it was played on");
}

void test_a_note_plays_the_chip_its_patch_copy_names() {
    // Core 0: a note on SID Patch A's load, then a Program Change to AY Patch
    // B replaces A's cook before it was ever published. The note starts on
    // B, so it must play as an AY note.
    Rig raced;
    raced.load_patch(0, factory(kPwmPluck));
    Samples got;
    raced.render(got);
    raced.load[0] = 2;            // Core 0 reloads A (load 2) -- never published --
    raced.note_on(0, 0, 60, 1);   // plays a note on it, as a SID note,
    raced.publish(0, factory(kAyLead), 3);  // and loads B (load 3), which is
    got.clear();
    raced.render(got, 6);

    Rig direct;
    direct.load_patch(0, factory(kPwmPluck));
    Samples want;
    direct.render(want);
    direct.load_patch(0, factory(kAyLead));
    direct.note_on(0, 0);
    want.clear();
    direct.render(want, 6);
    check(got == want && !silent(got), "a note whose Patch changed chip before Core 1 took it plays that chip");
}

void test_a_note_routes_through_a_filter_only_if_its_patch_copy_filters() {
    // Core 0 bound a bus for a filtered cook; the cook the note starts on
    // has FILTER off. It plays dry rather than through a bus with no mode.
    Samples got, want;
    Rig bound, dry;
    for (Rig *r : { &bound, &dry }) {
        r->load_patch(0, factory(kPwmPluck));
        r->note_on(0, 0);
    }
    bound.vp.voices[0].filter_bus = 0;
    bound.render(got, 6);
    dry.render(want, 6);
    check(got == want && !silent(got), "a note whose Patch copy doesn't filter plays dry, whatever bus it carries");
}

void test_the_part_volume_scales_the_note() {
    Samples full, half, none;
    Rig a, b, c;
    for (Rig *r : { &a, &b, &c }) {
        r->load_patch(3, factory(kAyLead));
        r->note_on(0, 3);
    }
    b.state->multi.part[3].volume_q15 = 16384;
    c.state->multi.part[3].volume_q15 = 0;
    a.render(full, 4);
    b.render(half, 4);
    c.render(none, 4);
    check(peak(half) * 10 < peak(full) * 7 && peak(half) * 10 > peak(full) * 3,
          "a Part at half volume plays its AY note at about half the level");
    check(silent(none), "a Part at volume 0 is silent");

    Samples chord;
    Rig pad;
    pad.load_patch(1, factory(kFilterPad));
    pad.note_on(0, 1, 48);
    pad.note_on(1, 1, 52);
    pad.state->multi.part[1].volume_q15 = 0;
    pad.render(chord, 4);
    check(silent(chord), "so is a filtered chord on its filter bus");
}

void test_the_part_pan_does_not_apply() {
    Samples centre, left;
    Rig a, b;
    for (Rig *r : { &a, &b }) {
        r->load_patch(0, factory(kArpLead));
        r->note_on(0, 0);
    }
    b.state->multi.part[0].pan_q15 = -32768;
    a.render(centre, 4);
    b.render(left, 4);
    check(left == centre && !silent(left), "chip is mono: a Part panned hard left sounds the same");
}

void test_a_part_sending_nothing_skips_the_fx() {
    Samples got, want;
    Rig dry, sent_none;
    for (Rig *r : { &dry, &sent_none }) {
        r->load_patch(0, factory(kPwmPluck));
        r->note_on(0, 0);
    }
    sent_none.state->multi.fx.type = FX_DELAY;
    sent_none.state->multi.fx.mix = 127;
    sent_none.state->multi.part[0].send_q15 = 0;
    dry.render(want, 6);
    sent_none.render(got, 6);
    check(got == want && !silent(got), "a Part with FX SEND 0 plays dry under a full-mix delay");
}

void test_a_note_released_while_waiting_for_its_cook_ends() {
    Rig rig;
    rig.load_patch(0, factory(kArpLead));
    rig.note_on(0, 0, 60, 1);  // its cook is still on Core 0
    Samples got;
    rig.render(got);
    rig.note_off(0);           // released before it ever started
    rig.edit_patch(0, factory(kArpLead));
    for (uint32_t i = 0; i < 400 && (i == 0 || (rig.state->active_mask & 1u)); i++) rig.render(got);
    check(!(rig.state->active_mask & 1u), "it starts, releases at once and ends");
}

}  // namespace

int main() {
    res2p_init();
    test_an_edit_waits_for_the_next_note();
    test_a_new_patch_leaves_a_sounding_note_on_its_own();
    test_a_note_waits_for_its_cook();
    test_a_note_played_just_before_a_program_change_keeps_its_patch();
    test_a_note_plays_the_chip_its_patch_copy_names();
    test_a_note_routes_through_a_filter_only_if_its_patch_copy_filters();
    test_the_part_volume_scales_the_note();
    test_the_part_pan_does_not_apply();
    test_a_part_sending_nothing_skips_the_fx();
    test_a_note_released_while_waiting_for_its_cook_ends();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
