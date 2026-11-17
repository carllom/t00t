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
    sub_note_start(vp, 0, (uint8_t)platform.cooked_generation(0),
                   sub_note_phase_inc(platform.cooked(0), note), 1.0f, velocity, 0, 0);
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

// `pc_after_note` >= 0: right after the first note-on is committed, and
// before Core 1 renders it, load that factory Patch into the Part -- a
// Program Change landing in the same buffer as the note.
std::vector<int16_t> render_patch(uint32_t patch, bool second_note = true, int pc_after_note = -1) {
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
        px.commit();
        if (b == 0 && pc_after_note >= 0) platform->load_factory(0, (uint32_t)pc_after_note);
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

    // A Program Change right behind a note-on must not reach that note: it
    // keeps the Patch (and the pitch Core 0 computed for it) it started
    // with. Fairlite (sample) interrupted by Sq-PWM (oscillator).
    bool pc_ok = render_patch(0, false) == render_patch(0, false, 1);
    printf("%s Program Change behind a note-on leaves that note on its own Patch\n", pc_ok ? "ok  " : "FAIL");

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
    bool ok = pc_ok && golden.size() == hashes.size();
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
