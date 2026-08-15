#pragma once

// The register stream format that both sides of the F0 comparison consume
// (module_chip.md §11.1). This header is that stream's parser, shared verbatim
// by tools/sid_ref/resid_render.cpp (reSID) and
// tools/host_render/render_sid.cpp (t00t/CHIP_STRICT) so the two renderers
// cannot disagree about *when* a write lands -- only about what it sounds
// like, which is the thing being measured.
//
// Format (text, one directive per line, '#' to end of line is a comment):
//
//   model 6581|8580        chip model                        (default 6581)
//   clock pal|ntsc|<hz>    master clock                      (default pal)
//   frame_cycles <n>       cycles per control frame          (default: the
//                          authentic raster frame for the selected clock)
//   frames <n>             total length of the render         (required)
//   @<frame> <reg> <val>   write $D4<reg> = $<val> at the start of that frame
//                          (reg and val are hex, reg 00-1c)
//
// Writes are applied at frame boundaries only (module_chip.md §6.2): the frame
// is the chip control clock, and a shared timebase coarser than either
// renderer -- reSID is cycle-accurate, t00t runs at 44.1 kHz -- is the only
// one both can honour exactly.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct SidRegWrite {
    uint32_t frame;
    uint8_t  reg;    // 0x00 - 0x1c ($D400 - $D41C)
    uint8_t  value;
};

struct SidRegStream {
    bool     model_8580  = false;
    double   clock_hz    = 985248.0;   // PAL
    uint32_t frame_cycles = 19656;     // PAL raster frame: 312 lines * 63 cycles
    uint32_t frames      = 0;
    std::vector<SidRegWrite> writes;   // sorted by frame (input order within a frame)

    // Control frame rate implied by the clock and the raster frame length.
    // PAL: 985248/19656 = 50.125 Hz -- the real one, not the nominal 50.
    double frame_rate() const { return clock_hz / (double)frame_cycles; }
};

// PAL/NTSC C64 master clocks and raster frame lengths.
static constexpr double   SIDREG_CLOCK_PAL    = 985248.0;
static constexpr uint32_t SIDREG_FRAME_PAL    = 19656;   // 312 lines * 63 cycles
static constexpr double   SIDREG_CLOCK_NTSC   = 1022730.0;
static constexpr uint32_t SIDREG_FRAME_NTSC   = 17095;   // 263 lines * 65 cycles

inline bool sidreg_parse(const std::string &path, SidRegStream &out, std::string &err) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) { err = "cannot open " + path; return false; }

    bool frame_cycles_explicit = false;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        // Strip comment and trailing whitespace.
        for (char *p = line; *p; p++) { if (*p == '#') { *p = '\0'; break; } }
        std::string s(line);
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = s.find_last_not_of(" \t\r\n");
        s = s.substr(b, e - b + 1);

        char a[64] = {0}, c[64] = {0}, d[64] = {0};
        int n = sscanf(s.c_str(), "%63s %63s %63s", a, c, d);
        if (n < 1) continue;

        std::string key(a);
        if (key == "model") {
            if (std::string(c) == "6581")      out.model_8580 = false;
            else if (std::string(c) == "8580") out.model_8580 = true;
            else { err = path + ":" + std::to_string(lineno) + ": model must be 6581 or 8580"; fclose(f); return false; }
        } else if (key == "clock") {
            std::string v(c);
            if (v == "pal") {
                out.clock_hz = SIDREG_CLOCK_PAL;
                if (!frame_cycles_explicit) out.frame_cycles = SIDREG_FRAME_PAL;
            } else if (v == "ntsc") {
                out.clock_hz = SIDREG_CLOCK_NTSC;
                if (!frame_cycles_explicit) out.frame_cycles = SIDREG_FRAME_NTSC;
            } else {
                out.clock_hz = atof(c);
                if (out.clock_hz <= 0.0) { err = path + ":" + std::to_string(lineno) + ": bad clock"; fclose(f); return false; }
            }
        } else if (key == "frame_cycles") {
            out.frame_cycles = (uint32_t)strtoul(c, nullptr, 0);
            frame_cycles_explicit = true;
            if (out.frame_cycles == 0) { err = path + ":" + std::to_string(lineno) + ": frame_cycles must be > 0"; fclose(f); return false; }
        } else if (key == "frames") {
            out.frames = (uint32_t)strtoul(c, nullptr, 0);
        } else if (key[0] == '@') {
            if (n < 3) { err = path + ":" + std::to_string(lineno) + ": expected '@<frame> <reg> <val>'"; fclose(f); return false; }
            SidRegWrite w;
            w.frame = (uint32_t)strtoul(key.c_str() + 1, nullptr, 10);
            w.reg   = (uint8_t)strtoul(c, nullptr, 16);
            w.value = (uint8_t)strtoul(d, nullptr, 16);
            if (w.reg > 0x1c) { err = path + ":" + std::to_string(lineno) + ": register out of range (00-1c)"; fclose(f); return false; }
            out.writes.push_back(w);
        } else {
            err = path + ":" + std::to_string(lineno) + ": unknown directive '" + key + "'";
            fclose(f); return false;
        }
    }
    fclose(f);

    if (out.frames == 0) { err = path + ": missing 'frames <n>'"; return false; }

    // Stable sort by frame, preserving file order within a frame -- a driver
    // that writes $D404 twice in one frame (gate off then on, the classic
    // retrigger) depends on the second write winning.
    for (size_t i = 1; i < out.writes.size(); i++) {
        SidRegWrite w = out.writes[i];
        size_t j = i;
        while (j > 0 && out.writes[j - 1].frame > w.frame) { out.writes[j] = out.writes[j - 1]; j--; }
        out.writes[j] = w;
    }
    return true;
}
