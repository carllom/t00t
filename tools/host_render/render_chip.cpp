// Render-equivalence check for the chip engine: plays a fixed note script
// for every factory Patch (and a few combined cases) through the device
// path -- stored Patch (factory_bank.h) -> decode -> cook -> Part slot ->
// render (src/engines/chip/chip_render.h) -- hashes the interleaved int16
// output, and compares against the golden hashes in golden/chip_render.txt.
//
// The golden hashes are those of the const instrument tables
// (instruments.h, ay_instruments.h) played directly, before the patch
// platform, so a match means the stored-format path sounds the same. Every
// case plays through Multi Parts at their default mix and the Multi's
// default FX (a delay at mix 0, as the pre-platform engine's default)
// unless the case sets them.
//
//   render_chip                         # compare, exit 1 on mismatch
//   render_chip --write-golden <file>   # (re)write the golden file
//   render_chip --wav <dir>             # also dump one WAV per case

#include "chip_factory.h"
#include "chip_render.h"
#include "instruments.h"
#include "ay_instruments.h"
#include "patch/multi.h"
#include "patch/platform.h"
#include "wav_writer.h"

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
constexpr uint32_t kProgramCount = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;

using ChipPlatform = PatchPlatformT<ChipModule>;
using ChipMulti = MultiT<ChipModule>;

// The Note Handler's note-on on `part`, centred bend, the Part's filter
// bus (if its Patch filters) being bus `part`.
struct Core0 {
    const ChipPlatform *platform;
    void start_note(VoiceParams &vp, uint8_t part, uint8_t note, uint8_t velocity) const {
        const ChipCookedPatch &c = platform->cooked(part);
        const bool ay = c.type == VT_AY;
        vp.type = c.type;
        const float hz = chip_note_to_hz(note);
        vp.sid_freq = chip_hz_to_freq_reg(hz);
        vp.ay_period = ay_hz_to_period(hz);
        vp.part = part;
        vp.load_id = platform->load_id(part);
        vp.cook_seq = platform->cook_seq(part);
        vp.velocity = velocity;
        vp.gate = true;
        vp.trigger++;
        vp.filter_bus = (!ay && c.uses_filter) ? part : BUS_NONE;
    }
};

struct Case {
    std::string name;
    std::vector<uint32_t> programs;  // Part n plays programs[n]; the other Parts are OFF
    // Core 0 activity in buffer `b`, before the voice commit.
    std::function<void(uint32_t b, const Core0 &core0, VoiceParamBlock &shadow)> script;
    std::function<void(ChipMulti &multi)> multi;  // Multi settings, before the first buffer
};

std::vector<Case> cases() {
    std::vector<Case> out;
    // Every instrument: C4 at 0, G4 at 40, both released at 120.
    for (uint32_t p = 0; p < kProgramCount; p++) {
        const char *name = p < INSTRUMENT_COUNT ? INSTRUMENT_NAMES[p] : AY_INSTRUMENT_NAMES[p - INSTRUMENT_COUNT];
        out.push_back({ name, { p }, [](uint32_t b, const Core0 &c0, VoiceParamBlock &s) {
            if (b == 0) c0.start_note(s.voices[0], 0, 60, 100);
            if (b == kSecondNoteBuffer) c0.start_note(s.voices[1], 0, 67, 127);
            if (b == kReleaseBuffer) s.voices[0].gate = s.voices[1].gate = false;
        }, nullptr });
    }
    // A chord on one filter bus.
    out.push_back({ "FILTER_PAD chord", { INS_FILTER_PAD }, [](uint32_t b, const Core0 &c0, VoiceParamBlock &s) {
        if (b == 0) {
            c0.start_note(s.voices[0], 0, 48, 100);
            c0.start_note(s.voices[1], 0, 52, 90);
            c0.start_note(s.voices[2], 0, 55, 80);
        }
        if (b == kReleaseBuffer) s.voices[0].gate = s.voices[1].gate = s.voices[2].gate = false;
    }, nullptr });
    // The FX insert: a delay at half mix.
    out.push_back({ "ARP_LEAD delay", { INS_ARP_LEAD }, [](uint32_t b, const Core0 &c0, VoiceParamBlock &s) {
        if (b == 0) c0.start_note(s.voices[0], 0, 60, 100);
        if (b == kReleaseBuffer) s.voices[0].gate = false;
    }, [](ChipMulti &m) {
        m.set_common(COMMON_P_FX_TYPE, FX_DELAY);
        m.set_common(COMMON_P_FX_MIX, 64);
        m.set_common(COMMON_P_FX_P1, 55);
        m.set_common(COMMON_P_FX_P2, 36);
    } });
    // SID and AY together on two Parts, through another speaker preset.
    out.push_back({ "PWM_PLUCK + AY ARP, GAMEBOY speaker", { INS_PWM_PLUCK, INSTRUMENT_COUNT + AY_INS_ARP },
                    [](uint32_t b, const Core0 &c0, VoiceParamBlock &s) {
        if (b == 0) {
            for (VoiceParams &v : s.voices) v.speaker_preset = SPEAKER_GAMEBOY;
            c0.start_note(s.voices[0], 0, 45, 110);
            c0.start_note(s.voices[1], 1, 64, 100);
        }
        if (b == kReleaseBuffer) s.voices[0].gate = s.voices[1].gate = false;
    }, nullptr });
    return out;
}

const FactoryBank kFactoryBanks[] = { { kChipFactoryBank, kChipFactoryCount } };

std::vector<int16_t> render_case(const Case &c) {
    ParamExchange px;
    px.init();
    auto state = std::make_unique<ChipRenderState>();
    chip_render_init(*state);
    auto slots = std::make_unique<PartSlotT<ChipCookedPatch>[]>(CHIP_PART_COUNT);
    auto platform = std::make_unique<ChipPlatform>(slots.get(), kFactoryBanks, 1, nullptr);
    auto multi_slot = std::make_unique<PartSlotT<ChipMultiCooked>>();
    auto multi = std::make_unique<ChipMulti>(*platform, nullptr, *multi_slot);

    // Part n plays the case's program n; every other Part is OFF.
    multi->init();
    for (uint8_t p = 0; p < CHIP_PART_COUNT; p++) {
        if (p >= c.programs.size()) { multi->set_part(p, PART_P_CHANNEL, kPartChannelOff); continue; }
        multi->set_part(p, PART_P_PROGRAM, (int32_t)c.programs[p]);
        if (multi->part_param(p, PART_P_PROGRAM) != (int32_t)c.programs[p]) {
            fprintf(stderr, "factory patch %u failed to load\n", c.programs[p]);
            exit(2);
        }
    }
    if (c.multi) c.multi(*multi);

    // Core 1 has already taken the Parts' Patches and the Multi before the
    // first note.
    for (int i = 0; i < 2; i++) {
        platform->task();
        multi->task();
        for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) chip_render_take_patch(*state, p, slots[p]);
        chip_render_take_multi(*state, *multi_slot);
    }

    const Core0 core0{ platform.get() };
    std::vector<int16_t> pcm;
    pcm.reserve(kBuffers * SAMPLES_PER_BUFFER * 2);
    int16_t buf[SAMPLES_PER_BUFFER * 2];
    for (uint32_t b = 0; b < kBuffers; b++) {
        platform->task();
        multi->task();
        VoiceParamBlock &shadow = px.shadow();
        shadow = px.active();
        c.script(b, core0, shadow);
        px.commit();
        // Core 1's per-buffer order: latch the voice block, take any new
        // cooked patches, render.
        const VoiceParamBlock &active = px.active();
        for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) chip_render_take_patch(*state, p, slots[p]);
        chip_render_take_multi(*state, *multi_slot);
        chip_render_buffer(*state, active, buf);
        pcm.insert(pcm.end(), buf, buf + SAMPLES_PER_BUFFER * 2);
    }
    return pcm;
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
    res2p_init();
    const char *write_path = nullptr;
    const char *wav_dir = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--write-golden")) write_path = argv[i + 1];
        if (!strcmp(argv[i], "--wav")) wav_dir = argv[i + 1];
    }

    const std::vector<Case> all = cases();
    std::vector<uint64_t> hashes;
    for (uint32_t i = 0; i < all.size(); i++) {
        const std::vector<int16_t> pcm = render_case(all[i]);
        hashes.push_back(fnv1a(pcm));
        if (wav_dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/chip_%02u.wav", wav_dir, i);
            write_wav_pcm16(path, pcm, SAMPLE_RATE, 2);
        }
    }

    if (write_path) {
        FILE *f = fopen(write_path, "w");
        if (!f) { perror(write_path); return 1; }
        fprintf(f, "# chip render-equivalence golden: <case> <FNV-1a 64 of int16 stereo>  # case name\n");
        fprintf(f, "# note script: C4 vel 100 at buffer 0, G4 vel 127 at buffer 40, release at 120, 300 buffers\n");
        for (uint32_t i = 0; i < all.size(); i++)
            fprintf(f, "%u %016llx  # %s\n", i, (unsigned long long)hashes[i], all[i].name.c_str());
        fclose(f);
        printf("wrote %zu hashes to %s\n", all.size(), write_path);
        return 0;
    }

    std::vector<uint64_t> golden;
    if (!read_golden(T00T_CHIP_GOLDEN, golden)) {
        fprintf(stderr, "cannot read %s\n", T00T_CHIP_GOLDEN);
        return 1;
    }
    bool ok = golden.size() == all.size();
    for (uint32_t i = 0; i < all.size(); i++) {
        const bool same = i < golden.size() && golden[i] == hashes[i];
        printf("%s %s\n", same ? "ok  " : "FAIL", all[i].name.c_str());
        ok = ok && same;
    }
    printf(ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return ok ? 0 : 1;
}
