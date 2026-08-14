// render_sid -- the t00t side of the F0 comparison (module_chip.md §11.1, CHIP_STRICT).
//
// Consumes a register stream (see sidreg.h) and renders it through
// src/chip/'s own primitives at 44.1 kHz, in the strict hardware topology:
// exactly 3 voices, 1 shared filter, adjacency-wired sync/ring (voice v's
// source is voice (v+2)%3, i.e. V1<-V3, V2<-V1, V3<-V2 -- reSID's own
// wiring). Free-routing mode (typed buses, per-voice sub-oscillator) cannot
// consume this stream at all -- that is the whole reason CHIP_STRICT exists
// as a second topology over the same validated primitives (module_chip.md §4, §11.1).
//
//   ./render_sid --stream streams/saw_sweep.sidreg --out out/test.wav
//
// tools/sid_ref/resid_render.cpp takes the identical CLI and renders the same
// stream through reSID; tools/sid_compare.py scores the difference.
//
// Host-only tooling, never linked into the device firmware.

#define CHIP_STRICT 1   // module_chip.md §11.1: velocity scaling must compile out here
#include "../sid_ref/sidreg.h"
#include "wav32.h"

#include "chip/sid_voice.h"
#include "chip/sid_filter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The strict 3-voice / 1-filter topology (module_chip.md §11.1's "$D400-$D418").
struct StrictSid {
    SidVoice   voice[3];
    SidFilter  filter;
    SidBoardFilter board;
    EnvSidRates rates;
    uint8_t    model;          // SidFilterModel: 0 = 6581, 1 = 8580

    uint16_t freq_reg[3] = {0, 0, 0};
    uint16_t pw_reg[3]   = {0, 0, 0};
    uint8_t  control[3]  = {0, 0, 0};

    uint16_t cutoff_reg = 0;   // 11-bit filter cutoff
    uint8_t  res_filt   = 0;   // $D417: resonance nibble + voice routing bits
    uint8_t  mode_vol   = 0;   // $D418: filter mode bits + voice3-off + volume

    void init(uint8_t chip_model, double clock_hz, double sample_rate) {
        model = chip_model;
        for (int v = 0; v < 3; v++) {
            voice[v].init(model);
            freq_reg[v] = 0;
            pw_reg[v] = 0;
            control[v] = 0;
        }
        filter.init();
        board.init();
        rates = env_sid_make_rates(clock_hz, sample_rate);
        cutoff_reg = 0;
        res_filt = 0;
        mode_vol = 0;
    }

    // Apply one $D400-$D418 register write.
    void write_reg(uint8_t reg, uint8_t val) {
        if (reg <= 0x14) {
            int v = reg / 7;
            uint8_t off = (uint8_t)(reg % 7);
            switch (off) {
                case 0x00: freq_reg[v] = (uint16_t)((freq_reg[v] & 0xff00) | val); break;
                case 0x01: freq_reg[v] = (uint16_t)((freq_reg[v] & 0x00ff) | ((uint16_t)val << 8)); break;
                case 0x02: pw_reg[v] = (uint16_t)((pw_reg[v] & 0x0f00) | val); break;
                case 0x03: pw_reg[v] = (uint16_t)((pw_reg[v] & 0x00ff) | ((uint16_t)(val & 0x0f) << 8)); break;
                case 0x04: control[v] = val; break;
                // AD/SR write only their own nibbles -- EnvSid keeps the four
                // ADSR fields separately, so set_adsr()'s "both bytes at
                // once" signature doesn't fit a register interface where
                // each byte is written independently.
                case 0x05: voice[v].env.attack  = (uint8_t)(val >> 4); voice[v].env.decay   = (uint8_t)(val & 0x0f); break;
                case 0x06: voice[v].env.sustain = (uint8_t)(val >> 4); voice[v].env.release = (uint8_t)(val & 0x0f); break;
            }
            voice[v].osc.pw = pw_reg[v];
            voice[v].osc.waveform = (uint8_t)((control[v] >> 4) & 0x0f);
            voice[v].osc.test = (uint8_t)((control[v] >> 3) & 0x01);
            // Gate bit drives ATTACK/RELEASE directly, hardware-literal --
            // NOT the instantaneous hard_restart() t00t's own P1 engine uses
            // for MIDI retriggering (module_chip.md §4.3's deliberate divergence).
            // CHIP_STRICT models the chip's actual register semantics, so a
            // gate rewrite while already gated must be a no-op here too.
            if (control[v] & 0x01) voice[v].env.gate_on();
            else                   voice[v].env.gate_off();
        } else if (reg == 0x15) {
            cutoff_reg = (uint16_t)((cutoff_reg & 0x7f8) | (val & 0x07));
        } else if (reg == 0x16) {
            cutoff_reg = (uint16_t)((cutoff_reg & 0x007) | ((uint16_t)val << 3));
        } else if (reg == 0x17) {
            res_filt = val;
        } else if (reg == 0x18) {
            mode_vol = val;
        }
        // 0x19-0x1c: paddle/osc3/env3 read-only registers -- not writable.
        for (int v = 0; v < 3; v++) {
            voice[v].osc.inc = sid_freq_to_inc(freq_reg[v], acc_scale);
        }
    }

    uint32_t acc_scale = 0;   // set by caller after init() (needs clock+rate)

    // One output sample, in the primitives' native (pre-mix-shift) units.
    int32_t tick() {
        // Phase 1: advance all three oscillators; capture MSB-rise for sync.
        bool rose[3];
        for (int v = 0; v < 3; v++) rose[v] = voice[v].osc.advance();

        // Phase 2: resolve hard sync from the adjacency-wired source.
        for (int v = 0; v < 3; v++) {
            int src = (v + 2) % 3;
            if ((control[v] & 0x02) && rose[src]) voice[v].osc.sync();
        }

        // Phase 3: read waveform output (with ring flip from the same
        // adjacency source) and tick the envelope.
        int32_t out[3];
        for (int v = 0; v < 3; v++) {
            int src = (v + 2) % 3;
            uint16_t ring_flip = (control[v] & 0x04) ? voice[src].osc.ring_flip_for_dest() : 0;
            out[v] = voice[v].output_and_tick_env(rates, ring_flip);
        }

        // Phase 4: route into the shared filter per $D417's bits 0-2; bit 7
        // of $D418 disconnects voice 3 entirely (filtered or not) -- the
        // classic "use voice 3 as a silent LFO/modulation source" trick.
        int32_t filt_sum = 0, dry_sum = 0;
        for (int v = 0; v < 3; v++) {
            if (v == 2 && (mode_vol & 0x80)) continue;
            if (res_filt & (1u << v)) filt_sum += out[v];
            else                      dry_sum += out[v];
        }

        int16_t f_half = sid_filter_f_half(model, cutoff_reg);
        int32_t q = sid_filter_q(model, (uint8_t)(res_filt >> 4));
        uint8_t mode_mask = (uint8_t)((mode_vol >> 4) & 0x07);
        int32_t filtered = filter.tick(filt_sum, f_half, q, mode_mask, /*saturate=*/true);

        int32_t mix = dry_sum + filtered;
        // 4-bit master volume DAC, linear (module_chip.md does not model this DAC's
        // nonlinearity anywhere -- level_gap_db is exactly the metric that
        // would catch it mattering).
        mix = (int32_t)(((int64_t)mix * (mode_vol & 0x0f)) / 15);

        return board.tick(mix);
    }
};

static void usage() {
    fprintf(stderr,
        "usage: render_sid --stream <file.sidreg> --out <file.wav> [options]\n"
        "  --stream <path>   register stream to render (required)\n"
        "  --out <path>      float32 analysis WAV (required)\n"
        "  --pcm16 <path>    additionally write a peak-normalised listening WAV\n"
        "  --rate <hz>       output sample rate (default 44100)\n"
        "  --fast            accepted for CLI parity with resid_render; no effect\n"
        "                    here -- t00t has no separate fast/resample mode\n");
}

int main(int argc, char **argv) {
    std::string stream_path, out_path, pcm16_path;
    double sample_rate = 44100.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "render_sid: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--stream") stream_path = next("--stream");
        else if (a == "--out")    out_path    = next("--out");
        else if (a == "--pcm16")  pcm16_path  = next("--pcm16");
        else if (a == "--rate")   sample_rate = atof(next("--rate").c_str());
        else if (a == "--fast")  {}
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "render_sid: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (stream_path.empty() || out_path.empty()) { usage(); return 2; }

    SidRegStream st;
    std::string err;
    if (!sidreg_parse(stream_path, st, err)) {
        fprintf(stderr, "render_sid: %s\n", err.c_str());
        return 1;
    }

    StrictSid sid;
    sid.init(st.model_8580 ? 1 : 0, st.clock_hz, sample_rate);
    sid.acc_scale = sid_acc_scale_q8(st.clock_hz, sample_rate);

    std::vector<float> samples;
    double frame_rate = st.frame_rate();
    samples.reserve((size_t)(st.frames * sample_rate / frame_rate) + 1024);

    double samples_per_frame = sample_rate / frame_rate;
    double sample_pos = 0.0;
    size_t wi = 0;

    for (uint32_t frame = 0; frame < st.frames; frame++) {
        while (wi < st.writes.size() && st.writes[wi].frame == frame) {
            sid.write_reg(st.writes[wi].reg, st.writes[wi].value);
            wi++;
        }
        double next_pos = sample_pos + samples_per_frame;
        uint32_t n = (uint32_t)next_pos - (uint32_t)sample_pos;
        for (uint32_t i = 0; i < n; i++) {
            int32_t raw = sid.tick();
            samples.push_back((float)(raw >> SID_MIX_SHIFT) / 32768.0f);
        }
        sample_pos = next_pos;
    }

    if (!write_wav_f32(out_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "render_sid: cannot write %s\n", out_path.c_str());
        return 1;
    }
    if (!pcm16_path.empty() && !write_wav_pcm16_normalized(pcm16_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "render_sid: cannot write %s\n", pcm16_path.c_str());
        return 1;
    }

    fprintf(stderr, "render_sid: %s -> %s  (%zu samples, %.3f s, %s, %.4f Hz frames)\n",
            stream_path.c_str(), out_path.c_str(), samples.size(),
            samples.size() / sample_rate, st.model_8580 ? "8580" : "6581", frame_rate);
    return 0;
}
