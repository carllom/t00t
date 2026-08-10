#pragma once

#include <cstdint>
#include "sid_voice.h"
#include "sid_filter.h"

// CHIP_STRICT: the real chip's topology, built out of the topology-free
// primitives.
//
// Three voices, one shared filter, sync and ring wired to the neighbouring
// voice, a 4-bit master volume. This is the *only* file in src/chip/ that
// knows any of that, and sid.md §3 discards every one of those limitations
// for the engine proper -- filter buses instead of one filter, a per-voice
// sub-oscillator instead of adjacency, a flat pool of 32 instead of 3.
//
// It exists because of §11.1: a register stream is $D400-$D418, free-routing
// mode cannot consume one, and "without a strict mode there is no way to prove
// the oscillator, envelope, LFSR and filter are correct at all". Validation
// targets the primitives; the routing above them is what differs between the
// two modes, and free mode inherits the primitives' correctness.
//
// Its second job is sid.md's P1 register-stream playback path, which is why
// this lives in src/chip/ rather than in the host harness.

struct SidChip {
    SidVoice voice[3];
    SidFilter filter;
    SidBoardFilter board;
    EnvSidRates rates;

    uint16_t fc;         // 11-bit cutoff register
    uint8_t  res;        // 4-bit resonance
    uint8_t  filt;       // routing bits: 0=voice1, 1=voice2, 2=voice3, 3=EXT
    uint8_t  mode;       // $D418 bits 7..4: 3OFF | HP | BP | LP
    uint8_t  vol;        // 4-bit master volume
    uint8_t  model;      // SidFilterModel

    void init(uint8_t chip_model, double clock_hz, double sample_rate) {
        for (int i = 0; i < 3; i++) voice[i].init(chip_model);
        filter.init();
        board.init();
        rates = env_sid_make_rates(clock_hz, sample_rate);
        fc = 0; res = 0; filt = 0; mode = 0; vol = 0;
        model = chip_model;
        acc_scale_q8 = sid_acc_scale_q8(clock_hz, sample_rate);
    }

    uint32_t acc_scale_q8;
    uint16_t freq_reg[3] = {0, 0, 0};

    // $D400-$D418. Registers above $D418 are write-only paddle/read registers
    // with no audio path and are accepted and ignored, so a stream captured
    // from a real driver does not need filtering first.
    void write(uint8_t reg, uint8_t value) {
        if (reg < 0x15) {
            int v = reg / 7;
            switch (reg % 7) {
                case 0: freq_reg[v] = (uint16_t)((freq_reg[v] & 0xff00) | value); break;
                case 1: freq_reg[v] = (uint16_t)((freq_reg[v] & 0x00ff) | (value << 8)); break;
                case 2: voice[v].osc.pw = (uint16_t)((voice[v].osc.pw & 0xf00) | value); break;
                case 3: voice[v].osc.pw = (uint16_t)((voice[v].osc.pw & 0x0ff) | ((value & 0x0f) << 8)); break;
                case 4: write_control(v, value); return;
                case 5: voice[v].env.set_adsr(value, (uint8_t)((voice[v].env.sustain << 4) | voice[v].env.release)); return;
                case 6: voice[v].env.set_adsr((uint8_t)((voice[v].env.attack << 4) | voice[v].env.decay), value); return;
            }
            if ((reg % 7) <= 1) voice[v].osc.inc = sid_freq_to_inc(freq_reg[v], acc_scale_q8);
            return;
        }
        switch (reg) {
            case 0x15: fc = (uint16_t)((fc & 0x7f8) | (value & 0x07)); break;
            case 0x16: fc = (uint16_t)(((value << 3) & 0x7f8) | (fc & 0x007)); break;
            case 0x17: res = (uint8_t)(value >> 4); filt = (uint8_t)(value & 0x0f); break;
            case 0x18: mode = (uint8_t)(value & 0xf0); vol = (uint8_t)(value & 0x0f); break;
            default: break;
        }
    }

    void write_control(int v, uint8_t value) {
        SidOsc &o = voice[v].osc;
        uint8_t test_prev = o.test;
        o.waveform = (uint8_t)(value >> 4);
        o.test     = (uint8_t)(value & 0x08);
        ring[v]    = (uint8_t)(value & 0x04);
        sync[v]    = (uint8_t)(value & 0x02);
        if (!test_prev && o.test) o.acc = 0;
        if (value & 0x01) voice[v].env.gate_on(); else voice[v].env.gate_off();
    }

    uint8_t ring[3] = {0, 0, 0};
    uint8_t sync[3] = {0, 0, 0};

    // One sample, in the strict topology.
    //
    // Output units: int16, matching reSID's SID::output(). The single scale
    // factor between the voice contract (sid_voice.h) and int16 lives here,
    // in SID_MIX_SHIFT, and is calibrated once against reSID rather than
    // tuned -- see sid_tables.h.
    inline int16_t tick() {
        // Phase 1: clock every accumulator, then resolve sync. Sync is wired to
        // the neighbour on real hardware -- voice N is synced by voice N-1,
        // wrapping -- so the edges have to be collected before any of them is
        // acted on, exactly as reSID clocks all three and only then calls
        // synchronize().
        bool rose[3];
        for (int v = 0; v < 3; v++) rose[v] = voice[v].osc.advance();
        for (int v = 0; v < 3; v++) {
            int src = (v + 2) % 3;   // voice 1 <- voice 3, voice 2 <- voice 1, ...
            if (sync[v] && rose[src]) voice[v].osc.sync();
        }

        // Phase 2: waveform x envelope, with ring taken from the same
        // neighbour. ring_flip_for_dest() is 0 when the source MSB is high,
        // and the flip only reaches the output when triangle is selected
        // without sawtooth -- sid_osc.h's combined-waveform path enforces that,
        // matching reSID's ring_msb_mask.
        int32_t vout[3];
        for (int v = 0; v < 3; v++) {
            int src = (v + 2) % 3;
            uint16_t flip = ring[v] ? voice[src].osc.ring_flip_for_dest() : 0x000;
            vout[v] = voice[v].output_and_tick_env(rates, flip);
        }

        // Phase 3: the filter's summing node, then the mixer.
        int32_t filtered_in = 0, direct = 0;
        for (int v = 0; v < 3; v++) {
            if (filt & (1 << v)) filtered_in += vout[v];
            else                 direct += vout[v];
        }
        // $D418 bit 7 mutes voice 3 -- but only when it is routed directly to
        // the mixer, not when it goes through the filter. Same rule as reSID's
        // set_sum_mix, and the reason a driver can use voice 3 for ENV3/OSC3
        // reads without hearing it.
        if ((mode & 0x80) && !(filt & 0x04)) direct -= vout[2];

        int32_t fout = filter.tick(filtered_in,
                                   sid_filter_f_half(model, fc),
                                   sid_filter_q(model, res),
                                   (uint8_t)((mode >> 4) & 0x07),
                                   model == SID_MODEL_6581);

        int32_t mixed = direct + fout;
        mixed = (mixed >> SID_MIX_SHIFT) * (int32_t)vol / 15;
        mixed = board.tick(mixed);

        if (mixed >  32767) mixed =  32767;
        if (mixed < -32768) mixed = -32768;
        return (int16_t)mixed;
    }
};
