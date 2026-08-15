#pragma once

// The AY-3-8910/YM2149 register stream format (same grammar and
// frame-boundary-only write rule as tools/sid_ref/sidreg.h). Shared verbatim
// by tools/ay_ref/ayumi_dump.cpp/ayumi_render.cpp (ayumi reference) and
// tools/host_render/render_ay.cpp (t00t/AY_STRICT).
//
// Format (text, one directive per line, '#' to end of line is a comment):
//
//   model ay|ym            chip model                        (default ay)
//   clock zx|msx|<hz>      master clock                      (default zx)
//   frame_cycles <n>       cycles per control frame           (default:
//                          clock/50 -- the AY has no raster tie, so this is
//                          a nominal 50 Hz frame)
//   frames <n>             total length of the render          (required)
//   @<frame> <reg> <val>   write AY register <reg> = <val> at that frame
//                          (reg and val are hex, reg 00-0d -- R0-R13;
//                          R14/R15 are I/O ports, out of scope here)
//
// Writes are applied at frame boundaries only (module_chip.md §6.2): the
// frame is the chip control clock, and a shared timebase coarser than either
// renderer is the only one both can honour exactly.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct AyRegWrite {
    uint32_t frame;
    uint8_t  reg;    // 0x00 - 0x0d (R0 - R13)
    uint8_t  value;
};

struct AyRegStream {
    bool     is_ym       = false;
    double   clock_hz    = 1773400.0;   // ZX Spectrum
    uint32_t frame_cycles = 35468;      // clock/50, nominal
    uint32_t frames      = 0;
    std::vector<AyRegWrite> writes;     // sorted by frame (input order within a frame)

    double frame_rate() const { return clock_hz / (double)frame_cycles; }
};

static constexpr double AYREG_CLOCK_ZX  = 1773400.0;
static constexpr double AYREG_CLOCK_MSX = 1789772.0;

inline bool ayreg_parse(const std::string &path, AyRegStream &out, std::string &err) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) { err = "cannot open " + path; return false; }

    bool frame_cycles_explicit = false;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
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
            if (std::string(c) == "ay")      out.is_ym = false;
            else if (std::string(c) == "ym") out.is_ym = true;
            else { err = path + ":" + std::to_string(lineno) + ": model must be ay or ym"; fclose(f); return false; }
        } else if (key == "clock") {
            std::string v(c);
            if (v == "zx") {
                out.clock_hz = AYREG_CLOCK_ZX;
            } else if (v == "msx") {
                out.clock_hz = AYREG_CLOCK_MSX;
            } else {
                out.clock_hz = atof(c);
                if (out.clock_hz <= 0.0) { err = path + ":" + std::to_string(lineno) + ": bad clock"; fclose(f); return false; }
            }
            if (!frame_cycles_explicit) out.frame_cycles = (uint32_t)(out.clock_hz / 50.0 + 0.5);
        } else if (key == "frame_cycles") {
            out.frame_cycles = (uint32_t)strtoul(c, nullptr, 0);
            frame_cycles_explicit = true;
            if (out.frame_cycles == 0) { err = path + ":" + std::to_string(lineno) + ": frame_cycles must be > 0"; fclose(f); return false; }
        } else if (key == "frames") {
            out.frames = (uint32_t)strtoul(c, nullptr, 0);
        } else if (key[0] == '@') {
            if (n < 3) { err = path + ":" + std::to_string(lineno) + ": expected '@<frame> <reg> <val>'"; fclose(f); return false; }
            AyRegWrite w;
            w.frame = (uint32_t)strtoul(key.c_str() + 1, nullptr, 10);
            w.reg   = (uint8_t)strtoul(c, nullptr, 16);
            w.value = (uint8_t)strtoul(d, nullptr, 16);
            if (w.reg > 0x0d) { err = path + ":" + std::to_string(lineno) + ": register out of range (00-0d)"; fclose(f); return false; }
            out.writes.push_back(w);
        } else {
            err = path + ":" + std::to_string(lineno) + ": unknown directive '" + key + "'";
            fclose(f); return false;
        }
    }
    fclose(f);

    if (out.frames == 0) { err = path + ": missing 'frames <n>'"; return false; }

    for (size_t i = 1; i < out.writes.size(); i++) {
        AyRegWrite w = out.writes[i];
        size_t j = i;
        while (j > 0 && out.writes[j - 1].frame > w.frame) { out.writes[j] = out.writes[j - 1]; j--; }
        out.writes[j] = w;
    }
    return true;
}
