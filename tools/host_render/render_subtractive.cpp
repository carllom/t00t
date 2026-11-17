// Render-equivalence check for the subtractive engine: renders every
// factory patch through the device path -- stored Patch (factory_bank.h)
// -> decode -> cook -> Part slot -> render (src/engines/subtractive/
// render.h) -- with a fixed note script, hashes the interleaved int16
// output, and compares against the golden hashes in
// golden/subtractive_render.txt.
//
//   render_subtractive                         # compare, exit 1 on mismatch
//   render_subtractive --write-golden <file>   # (re)write the golden file
//   render_subtractive --wav <dir>             # also dump one WAV per patch
//
// The golden hashes are those of the const-table presets (presets.h)
// played directly, so a match means the stored-format path sounds the same.

#include "factory_bank.h"
#include "patch.h"
#include "patch/platform.h"
#include "render.h"
#include "osc/sample.h"
#include "wav_writer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kBuffers = 300;           // ~1.74 s
constexpr uint32_t kSecondNoteBuffer = 40;   // ~0.23 s
constexpr uint32_t kReleaseBuffer = 120;     // ~0.70 s

using SubPlatform = PatchPlatformT<SubtractiveModule>;

// The Note Handler's note-on on Part 0, centred bend/pan, no mod wheel.
void start_note(VoiceParams &vp, const SubPlatform &platform, uint8_t note, uint8_t velocity) {
    const SubCookedPatch &patch = platform.cooked(0);
    sub_note_start(vp, 0, patch, platform.load_id(0), sub_note_phase_inc(patch, note), 1.0f, velocity, 0, 0);
}

uint64_t fnv1a(const std::vector<int16_t> &pcm) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int16_t s : pcm) {
        uint16_t u = (uint16_t)s;
        for (int b = 0; b < 2; b++) {
            h ^= (uint8_t)(u >> (8 * b));
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

// Core 0 activity injected into buffer `b`: `before` runs before the
// buffer's voice commit and may start notes in `shadow`; `after` runs after
// the commit, before Core 1 renders (a Program Change or edit landing
// behind the buffer's notes).
struct Hooks {
    std::function<void(uint32_t b, SubPlatform &platform, VoiceParamBlock &shadow)> before;
    std::function<void(uint32_t b, SubPlatform &platform)> after;
};

std::vector<int16_t> render_patch(uint32_t patch, bool second_note = true, const Hooks &hooks = {}) {
    ParamExchange px;
    px.init();
    auto state = std::make_unique<SubRenderState>();
    sub_render_init(*state);
    auto slots = std::make_unique<PartSlotT<SubCookedPatch>[]>(SUB_PART_COUNT);
    auto platform = std::make_unique<SubPlatform>(slots.get(), kSubFactoryBank, kSubFactoryCount);
    if (platform->load_factory(0, patch) != PatchLoadResult::OK) {
        fprintf(stderr, "factory patch %u failed to load\n", patch);
        exit(2);
    }

    // Core 1 has already taken the Part's Patch before the first note.
    for (uint32_t p = 0; p < SUB_PART_COUNT; p++) sub_render_take_patch(*state, p, slots[p]);

    std::vector<int16_t> pcm;
    pcm.reserve(kBuffers * SAMPLES_PER_BUFFER * 2);
    int16_t buf[SAMPLES_PER_BUFFER * 2];

    for (uint32_t b = 0; b < kBuffers; b++) {
        platform->task();
        VoiceParamBlock &shadow = px.shadow();
        shadow = px.active();
        if (b == 0) start_note(shadow.voices[0], *platform, 60, 100);
        if (b == kSecondNoteBuffer && second_note) start_note(shadow.voices[1], *platform, 67, 127);
        if (b == kReleaseBuffer) {
            shadow.voices[0].gate = false;
            shadow.voices[1].gate = false;
        }
        if (hooks.before) hooks.before(b, *platform, shadow);
        px.commit();
        if (hooks.after) hooks.after(b, *platform);
        // Core 1's per-buffer order: latch the voice block, take any new
        // cooked patches, render.
        const VoiceParamBlock &active = px.active();
        for (uint32_t p = 0; p < SUB_PART_COUNT; p++) sub_render_take_patch(*state, p, slots[p]);
        sub_render_buffer(*state, active, buf);
        pcm.insert(pcm.end(), buf, buf + SAMPLES_PER_BUFFER * 2);
    }
    return pcm;
}

bool read_golden(const char *path, std::vector<uint64_t> &out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned idx;
        unsigned long long h;
        if (sscanf(line, "%u %llx", &idx, &h) != 2) continue;
        if (idx >= out.size()) out.resize(idx + 1, 0);
        out[idx] = h;
    }
    fclose(f);
    return true;
}

// Index of the first buffer where `a` and `b` differ, or -1 if identical.
int first_difference(const std::vector<int16_t> &a, const std::vector<int16_t> &b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        if (a[i] != b[i]) return (int)(i / (SAMPLES_PER_BUFFER * 2));
    }
    return a.size() == b.size() ? -1 : 0;
}

bool report(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

// Each parameter's cooked fields sit in the group its Parameter class says:
// changing a live parameter leaves `note_on` untouched and vice versa.
bool cook_groups_match_classes() {
    bool ok = true;
    for (uint32_t id = 0; id < SUB_P_COUNT; id++) {
        const ParamDescriptor &d = SubtractiveModule::kDescriptors[id];
        int32_t v[SUB_P_COUNT];
        for (uint32_t i = 0; i < SUB_P_COUNT; i++) v[i] = SubtractiveModule::kDescriptors[i].def;
        v[SUB_P_WAVEFORM] = WAVE_SAMPLE;      // so SAMPLE has an effect
        v[SUB_P_FILTER_MODE] = FILTER_LP;
        SubCookedPatch a{}, b{};
        SubtractiveModule::cook(v, a);
        v[id] = (v[id] == d.max) ? d.min : d.max;
        SubtractiveModule::cook(v, b);
        bool note_on_same = memcmp(&a.note_on, &b.note_on, sizeof a.note_on) == 0;
        bool live_same = memcmp(&a.live, &b.live, sizeof a.live) == 0;
        bool good = (d.cls == ParamClass::LIVE) ? note_on_same && !live_same
                                                : live_same && !note_on_same;
        if (!good) printf("FAIL %s: cooked group doesn't match its class\n", d.name);
        ok = ok && good;
    }
    return ok;
}

// Saw-Flt's stored resonance, for an edit that changes nothing audible.
constexpr int32_t presets_reso_saw_flt = 24000;

// Edits arrive as MIDI Control Changes on channel 1 (Part 0).
void cc(SubPlatform &p, uint8_t cc, uint8_t value) { p.midi_cc(0, cc, value); }
void nrpn(SubPlatform &p, uint16_t id, uint8_t msb) {
    cc(p, 99, (uint8_t)(id >> 7));
    cc(p, 98, (uint8_t)(id & 0x7F));
    cc(p, 6, msb);
}
// NRPN Data Entry MSB that lands on enum value `v` of a 0..max parameter.
uint8_t enum_msb(uint32_t v, uint32_t max) { return (uint8_t)((v * 127 + max / 2) / max); }

Hooks after_at(uint32_t at, void (*edit)(SubPlatform &)) {
    return { nullptr, [at, edit](uint32_t b, SubPlatform &p) { if (b == at) edit(p); } };
}

bool behaviour_checks() {
    bool ok = report(cook_groups_match_classes(), "every parameter cooks into its class's group");

    // Saw-Flt (patch 2), first note only; edits land before buffer 20.
    const std::vector<int16_t> base = render_patch(2, false);

    int live = first_difference(base, render_patch(2, false, after_at(20, [](SubPlatform &p) {
        cc(p, 74, 40);  // CUTOFF
    })));
    ok = report(live == 20, "live CUTOFF edit (CC74) changes the sounding note from the next buffer") && ok;

    int note_on = first_difference(base, render_patch(2, false, after_at(20, [](SubPlatform &p) {
        nrpn(p, SUB_P_WAVEFORM, enum_msb(WAVE_SQUARE_BLEP, WAVE_SAMPLE));
    })));
    ok = report(note_on == -1, "note-on WAVE edit (NRPN) leaves the sounding note as it started") && ok;

    int pc_held = first_difference(base, render_patch(2, false, after_at(20, [](SubPlatform &p) {
        p.load_factory(0, 1);
    })));
    ok = report(pc_held == -1, "Program Change leaves held notes on their Patch") && ok;

    // A Program Change right behind a note-on must not reach that note: it
    // keeps the Patch (and the pitch Core 0 computed for it) it started
    // with. Fairlite (sample) interrupted by Sq-PWM (oscillator).
    int pc_same_buffer = first_difference(render_patch(0, false), render_patch(0, false,
        after_at(0, [](SubPlatform &p) { p.load_factory(0, 1); })));
    ok = report(pc_same_buffer == -1, "Program Change behind a note-on leaves that note on its own Patch") && ok;

    // After a Program Change, a live edit reaches only notes on the new Patch.
    int edit_after_pc = first_difference(base, render_patch(2, false, { nullptr, [](uint32_t b, SubPlatform &p) {
        if (b == 20) p.load_factory(0, 1);
        if (b == 30) cc(p, 74, 40);
    } }));
    ok = report(edit_after_pc == -1, "an edit after a Program Change doesn't reach notes on the old Patch") && ok;

    // A note-on edit queued behind another edit (the slot is busy) still
    // reaches a note started after it in the same buffer: compare with the
    // same WAVE edit made well before the second note.
    auto second_note_square = [](uint32_t edit_at, bool block_slot) {
        return render_patch(2, false, { [edit_at, block_slot](uint32_t b, SubPlatform &p, VoiceParamBlock &shadow) {
            if (b == edit_at) {
                if (block_slot) p.set_param(0, SUB_P_RESONANCE, presets_reso_saw_flt);  // unchanged value: just occupies the slot
                nrpn(p, SUB_P_WAVEFORM, enum_msb(WAVE_SQUARE_BLEP, WAVE_SAMPLE));
            }
            if (b == 40) start_note(shadow.voices[1], p, 67, 127);
        }, nullptr });
    };
    int queued = first_difference(second_note_square(10, false), second_note_square(40, true));
    ok = report(queued == -1, "a note right after a queued note-on edit plays the edited Patch") && ok;
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    const char *golden_path = T00T_SUBTRACTIVE_GOLDEN;
    const char *write_path = nullptr;
    const char *wav_dir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--write-golden") && i + 1 < argc) write_path = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_dir = argv[++i];
        else { fprintf(stderr, "usage: %s [--write-golden <file>] [--wav <dir>]\n", argv[0]); return 2; }
    }

    osc_init_sine();

    std::vector<uint64_t> hashes;
    for (uint32_t p = 0; p < kSubFactoryCount; p++) {
        std::vector<int16_t> pcm = render_patch(p);
        hashes.push_back(fnv1a(pcm));
        if (wav_dir) {
            write_wav_pcm16(std::string(wav_dir) + "/subtractive_patch" + std::to_string(p) + ".wav",
                            pcm, SAMPLE_RATE, 2);
        }
    }

    bool behaviour_ok = behaviour_checks();

    if (write_path) {
        FILE *f = fopen(write_path, "w");
        if (!f) { perror(write_path); return 2; }
        fprintf(f, "# subtractive render-equivalence golden: <patch index> <FNV-1a 64 of int16 stereo>\n");
        fprintf(f, "# note script: C4 vel 100 at buffer 0, G4 vel 127 at buffer %u, release at %u, %u buffers\n",
                kSecondNoteBuffer, kReleaseBuffer, kBuffers);
        for (uint32_t p = 0; p < hashes.size(); p++) {
            fprintf(f, "%u %016llx\n", p, (unsigned long long)hashes[p]);
        }
        fclose(f);
        printf("wrote %zu hashes to %s\n", hashes.size(), write_path);
        return 0;
    }

    std::vector<uint64_t> golden;
    if (!read_golden(golden_path, golden)) {
        fprintf(stderr, "cannot read golden file %s\n", golden_path);
        return 2;
    }
    bool ok = behaviour_ok && golden.size() == hashes.size();
    if (golden.size() != hashes.size()) printf("FAIL patch count: golden %zu, rendered %zu\n", golden.size(), hashes.size());
    for (uint32_t p = 0; p < hashes.size() && p < golden.size(); p++) {
        bool match = hashes[p] == golden[p];
        printf("%s patch %u: %016llx%s\n", match ? "ok  " : "FAIL", p,
               (unsigned long long)hashes[p], match ? "" : " (golden differs)");
        ok = ok && match;
    }
    printf(ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return ok ? 0 : 1;
}
