// nuked_render -- OPL2 reference renderer, driving Nuked-OPL3 (nuked/opl3.c,
// fetched by fetch_nuked_opl3.sh) directly through its real register
// interface (OPL3_WriteReg), the same way a real OPL2 chip is programmed.
//
// Takes one operator pair's worth of OPL2 register fields (mult/ksl/tl/ar/
// dr/sl/rr/egt/ksr/ws, matching src/engines/opl/patch.h's OplOpParams field
// for field) plus feedback/algorithm/note/velocity, writes them to channel 0
// (operator slots 0x00 and 0x03 -- confirmed against nuked/opl3.c's ad_slot[]
// table), keys the note on, renders `--gate` seconds, keys it off, renders
// `--tail` more, and writes a stereo PCM16 WAV.
//
// tools/host_render/render_opl.cpp renders the same kind of patch through
// t00t's own engine; nothing here is ever linked into device firmware.
//
// Nuked-OPL3 has no velocity register at all -- real OPL2 hardware doesn't
// either. Velocity is folded onto TL here using the exact same formula
// src/engines/opl/env_opl.h's env_opl_init() uses, so a rendered comparison
// at a given velocity means the same thing on both sides.
//
// Build: make (see Makefile). Run: ./nuked_render --help

#include "opl3.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr double SR = 44100.0;
static constexpr double OPL_CLOCK_OVER_72 = 49716.0;  // real OPL2 clock / 72, F-num/Block reference

struct OpFields {
    int mult = 1, ksl = 0, tl = 0, ar = 15, dr = 8, sl = 0, rr = 8, egt = 1, ksr = 0, ws = 0;
};

static void parse_op(const char *s, OpFields &op) {
    if (sscanf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &op.mult, &op.ksl, &op.tl, &op.ar, &op.dr,
               &op.sl, &op.rr, &op.egt, &op.ksr, &op.ws) != 10) {
        fprintf(stderr, "error: expected mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws -- got '%s'\n", s);
        exit(2);
    }
}

// note-on-time output-level composition (env_opl.h's env_opl_init): TL is
// adjusted by a velocity-derived attenuation before it ever reaches the
// register -- there is no separate velocity register to write.
static int velocity_adjusted_tl(int tl, int velocity) {
    double vel_db = (double)(127 - velocity) * 0.375;
    int adjusted = tl + (int)lround(vel_db / 0.75);
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 63) adjusted = 63;
    return adjusted;
}

static void write_op(opl3_chip &chip, int slot_offset, const OpFields &op, int tl_override) {
    uint8_t r20 = (uint8_t)(((op.egt & 1) << 5) | ((op.ksr & 1) << 4) | (op.mult & 0x0F));
    OPL3_WriteReg(&chip, 0x20 + slot_offset, r20);
    uint8_t r40 = (uint8_t)(((op.ksl & 3) << 6) | (tl_override & 0x3F));
    OPL3_WriteReg(&chip, 0x40 + slot_offset, r40);
    uint8_t r60 = (uint8_t)(((op.ar & 0x0F) << 4) | (op.dr & 0x0F));
    OPL3_WriteReg(&chip, 0x60 + slot_offset, r60);
    uint8_t r80 = (uint8_t)(((op.sl & 0x0F) << 4) | (op.rr & 0x0F));
    OPL3_WriteReg(&chip, 0x80 + slot_offset, r80);
    OPL3_WriteReg(&chip, 0xE0 + slot_offset, (uint8_t)(op.ws & 0x03));
}

// Largest F-Num/Block pair (10-bit Fnum, 3-bit Block) that reproduces
// freq_hz on real OPL2 hardware, standard chip-clock/72 formula.
static void freq_to_fnum_block(double freq_hz, uint16_t &fnum, uint8_t &block) {
    for (int b = 0; b <= 7; b++) {
        double f = freq_hz * (double)(1u << (20 - b)) / OPL_CLOCK_OVER_72;
        if (f <= 1023.0) { block = (uint8_t)b; fnum = (uint16_t)lround(f); return; }
    }
    block = 7;
    fnum = 1023;
}

static void write_note(opl3_chip &chip, uint16_t fnum, uint8_t block, bool key_on) {
    OPL3_WriteReg(&chip, 0xA0, (uint8_t)(fnum & 0xFF));
    uint8_t rb0 = (uint8_t)((key_on ? 0x20 : 0) | ((block & 7) << 2) | ((fnum >> 8) & 3));
    OPL3_WriteReg(&chip, 0xB0, rb0);
}

static void usage() {
    printf(
        "nuked_render -- OPL2 reference renderer (Nuked-OPL3)\n"
        "\n"
        "  --op0 mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws   modulator (default 1,0,0,15,8,0,8,1,0,0)\n"
        "  --op1 mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws   carrier   (default 1,0,0,15,8,0,8,1,0,0)\n"
        "  --feedback N      0-7 (default 0)\n"
        "  --algo fm|add     FM chain or additive (default fm)\n"
        "  --note N          MIDI note number (default 57 = A3)\n"
        "  --vel N           MIDI velocity 1-127 (default 127)\n"
        "  --gate SEC        seconds to hold the note (default 1.0)\n"
        "  --tail SEC        seconds to render after key-up (default 2.0)\n"
        "  --out PATH        stereo PCM16 WAV (default ref.wav)\n");
}

int main(int argc, char **argv) {
    OpFields op0, op1;
    int feedback = 0, note = 57, velocity = 127;
    bool additive = false;
    double gate = 1.0, tail = 2.0;
    std::string out = "ref.wav";

    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--op0")       parse_op(next(), op0);
        else if (k == "--op1")       parse_op(next(), op1);
        else if (k == "--feedback")  feedback = atoi(next());
        else if (k == "--algo")      additive = (std::string(next()) == "add");
        else if (k == "--note")      note = atoi(next());
        else if (k == "--vel")       velocity = atoi(next());
        else if (k == "--gate")      gate = atof(next());
        else if (k == "--tail")      tail = atof(next());
        else if (k == "--out")       out = next();
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }

    opl3_chip chip;
    OPL3_Reset(&chip, (uint32_t)SR);

    int tl0 = velocity_adjusted_tl(op0.tl, velocity);
    int tl1 = velocity_adjusted_tl(op1.tl, velocity);
    write_op(chip, 0x00, op0, tl0);
    write_op(chip, 0x03, op1, tl1);
    OPL3_WriteReg(&chip, 0xC0, (uint8_t)(((feedback & 7) << 1) | (additive ? 1 : 0)));

    double freq_hz = 440.0 * pow(2.0, (double)(note - 69) / 12.0);
    uint16_t fnum;
    uint8_t block;
    freq_to_fnum_block(freq_hz, fnum, block);
    write_note(chip, fnum, block, true);

    uint32_t gate_frames = (uint32_t)(gate * SR);
    uint32_t total_frames = (uint32_t)((gate + tail) * SR);

    std::vector<int16_t> stereo(total_frames * 2);
    OPL3_GenerateStream(&chip, stereo.data(), gate_frames);
    write_note(chip, fnum, block, false);
    OPL3_GenerateStream(&chip, stereo.data() + (size_t)gate_frames * 2, total_frames - gate_frames);

    if (!write_wav_pcm16(out, stereo, (uint32_t)SR, 2)) {
        fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }

    int16_t peak = 0;
    for (int16_t s : stereo) peak = std::max(peak, (int16_t)std::abs((int)s));
    printf("nuked_render: note %d vel %d gate %.2fs tail %.2fs -> fnum %u block %u\n",
           note, velocity, gate, tail, fnum, block);
    printf("  wrote %s -- %u frames, peak %d\n", out.c_str(), total_frames, peak);
    return 0;
}
