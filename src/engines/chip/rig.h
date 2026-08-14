#pragma once

#include <cstdint>

#include "chip/sid_voice.h"
#include "chip/sid_filter.h"

// Chip module measurement rig (module_chip.md §1).
//
// This is a self-contained rig, not an engine and not a modification of
// one: N voices with fixed parameters, the §7.2 two-phase bus render, no
// MIDI, no frame VM, no allocation, no display. It produces the numbers
// the next slice reads; it decides nothing itself.
//
// Every lever this rig measures is a compile-time switch, so a
// measurement session is a sequence of builds rather than a runtime menu
// (the value being measured is cycles per frame, and a runtime switch
// would put a branch inside the thing under test):
//
//   CHIP_RIG_VOICES      total voices rendered            (default 20, §9's target)
//   CHIP_RIG_BUSES       filter bus count                 (default 4,  §13.3)
//   CHIP_RIG_FILTERED    voices routed into a bus         (default 12, §9)
//   CHIP_RIG_OVERSAMPLE  oscillator oversampling, 1 or 2
//   CHIP_RIG_WAVE_DAC    12-bit waveform DAC LUT on/off
//   CHIP_RIG_SAT         6581 filter saturation on/off
//   CHIP_RIG_SUBBLOCK    sub-block length                 (default 64, §7.2)
//   CHIP_RIG_MOD         per-voice sub-oscillator mode: 0 off, 1 sync, 2 ring
//   CHIP_RIG_FX          post-mix insert: 0 off, 1 delay, 2 reverb (§10, fx/)
//   CHIP_RIG_SPEAKER     §10 speaker sim stage on/off, downstream of FX
//
// The four measurements this rig produces, and how to take them:
//
//   per-voice cost          diff CHIP_RIG_VOICES=24 against =0 (idle), /24
//   filtered-voice cost     diff CHIP_RIG_FILTERED=12 against =0 at the same
//   *including the bus       voice count. The round trip is the store into the
//   round trip*             bus accumulator plus the load back out, and §7.2
//                           requires it be inside the number, not beside it
//   sync at 1x vs 2x        diff CHIP_RIG_OVERSAMPLE=2 against =1 with
//                           CHIP_RIG_MOD=1
//   waveform DAC            diff CHIP_RIG_WAVE_DAC=1 against =0
//
// The waveform DAC isn't in module_chip.md -- the 12-bit table is 8 KB of
// flash, so its cost wants a number rather than an opinion.

#ifndef CHIP_RIG_VOICES
#define CHIP_RIG_VOICES 20  // module_chip.md §9's target voice/bus split
#endif
#ifndef CHIP_RIG_BUSES
#define CHIP_RIG_BUSES 4
#endif
#ifndef CHIP_RIG_FILTERED
#define CHIP_RIG_FILTERED 12
#endif
#ifndef CHIP_RIG_OVERSAMPLE
#define CHIP_RIG_OVERSAMPLE 1
#endif
#ifndef CHIP_RIG_SAT
#define CHIP_RIG_SAT 1
#endif
#ifndef CHIP_RIG_SUBBLOCK
#define CHIP_RIG_SUBBLOCK 64
#endif
#ifndef CHIP_RIG_MOD
#define CHIP_RIG_MOD 0
#endif
#ifndef CHIP_RIG_FX
#define CHIP_RIG_FX 0
#endif
#ifndef CHIP_RIG_SPEAKER
#define CHIP_RIG_SPEAKER 0
#endif
// ADSR: decay rate 7 (313 c/step) toward a mid sustain level. Not decay=0
// -- decay=0 is the *fastest* rate period (9 cycles, same as attack), and
// reaching sustain only stops the counter; EnvSid::tick()'s phase
// accumulator keeps advancing at that rate regardless, so a decay=0 voice
// re-enters the per-sample while-loop several times per sample for the
// life of the note (see env_sid.h's tick() comment). This settles to
// sustain within well under a second, so almost all of each 4s phase
// measures the steady-state cost a real held note actually pays.

static_assert(CHIP_RIG_FILTERED <= CHIP_RIG_VOICES,
              "cannot route more voices into buses than the rig renders");
static_assert(CHIP_RIG_OVERSAMPLE == 1 || CHIP_RIG_OVERSAMPLE == 2,
              "oversampling is 1x or 2x");

// The host build never defines the pico-sdk macro; device builds pull it
// in from pico/platform.h first.
#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

// §7.2's bus accumulators: FILTER_BUS_COUNT x SUBBLOCK x int32. At the
// defaults that is 4 x 64 x 4 = 1 KB, not buffer-sized -- this is why the
// sub-block cut point matters here.
struct ChipRig {
    SidVoice   voice[CHIP_RIG_VOICES > 0 ? CHIP_RIG_VOICES : 1];
    SidOsc     mod[CHIP_RIG_VOICES > 0 ? CHIP_RIG_VOICES : 1];  // §4.4 sub-oscillator
    SidFilter  bus[CHIP_RIG_BUSES > 0 ? CHIP_RIG_BUSES : 1];
    int32_t    bus_acc[CHIP_RIG_BUSES > 0 ? CHIP_RIG_BUSES : 1][CHIP_RIG_SUBBLOCK];
    EnvSidRates rates;

    void init() {
        rates = env_sid_make_rates(SID_CLOCK_PAL, SAMPLE_RATE);
        uint32_t scale = sid_acc_scale_q8(SID_CLOCK_PAL, SAMPLE_RATE);
        for (uint32_t v = 0; v < CHIP_RIG_VOICES; v++) {
            voice[v].init(SID_MODEL_6581);
            // Spread the pitches so no two voices share an accumulator phase
            // and the noise register clock counts differ -- a rig where every
            // voice runs identically measures one voice N times and hides any
            // data-dependent cost.
            voice[v].osc.inc = sid_freq_to_inc((uint16_t)(2000 + v * 613), scale);
            voice[v].osc.pw = (uint16_t)(0x400 + v * 37);
            // Sawtooth+pulse, so both the comparator and the combined path are
            // exercised; noise on every fourth voice for its extra LFSR work.
            voice[v].osc.waveform = (v % 4 == 3) ? SID_WAVE_NOISE
                                                 : (SID_WAVE_SAW | SID_WAVE_PULSE);
            voice[v].env.set_adsr(0x07, 0x80);   // decay 7 (313 c/step) to mid sustain
            voice[v].env.gate_on();

            mod[v].init();
            mod[v].inc = sid_freq_to_inc((uint16_t)(1500 + v * 271), scale);
        }
        for (uint32_t b = 0; b < CHIP_RIG_BUSES; b++) bus[b].init();
    }

    // Render one sub-block into a dry mix, in the §7.2 two-phase shape.
    //
    //   per sub-block:
    //       clear bus accumulators
    //       for each active voice: render -> bus[v.filter_bus], or -> dry mix
    //       for each bound bus: filter its accumulator, sum into dry mix
    //
    // Voices 0..CHIP_RIG_FILTERED-1 go through a bus; the rest go straight to
    // the mix, so the unfiltered common case keeps its full speed and the
    // difference between two builds is exactly the round trip.
    // `active` is a *runtime* count so one build can sweep it. The
    // per-voice cost is the slope across that sweep, and taking it from
    // two builds' intercepts instead would fold their code layout
    // differences into it.
    void __not_in_flash_func(render_n)(int32_t *dry, uint32_t n, uint32_t active) {
        if (active > CHIP_RIG_VOICES) active = CHIP_RIG_VOICES;
#if CHIP_RIG_BUSES > 0
        for (uint32_t b = 0; b < CHIP_RIG_BUSES; b++)
            for (uint32_t i = 0; i < n; i++) bus_acc[b][i] = 0;
#endif

        for (uint32_t v = 0; v < active; v++) {
            for (uint32_t i = 0; i < n; i++) {
                bool sync_reset = false;
                uint16_t ring_flip = 0;
#if CHIP_RIG_MOD != 0
                bool rose = mod[v].advance();
#if CHIP_RIG_MOD == 1
                sync_reset = rose;
#else
                (void)rose;
                ring_flip = mod[v].ring_flip_for_dest();
#endif
#endif
                int32_t s;
#if CHIP_RIG_OVERSAMPLE == 2
                // 2x: two oscillator steps per output sample, averaged. The
                // envelope stays at 1x -- it is a control-rate quantity and
                // doubling it would measure the wrong thing.
                voice[v].osc.advance();
                if (sync_reset) voice[v].osc.sync();
                uint16_t w0 = voice[v].osc.output(ring_flip);
                voice[v].osc.advance();
                uint16_t w1 = voice[v].osc.output(ring_flip);
                uint8_t e = voice[v].env.tick(rates);
                // The waveform DAC is applied per oscillator step, before the
                // average, and the envelope DAC once after -- so the only
                // difference between this build and the 1x one is the extra
                // oscillator step. Averaging raw phases and then looking up
                // the DAC would measure a cheaper, different thing and make
                // the CHIP_WAVE_DAC diff meaningless in 2x builds.
#if CHIP_WAVE_DAC
                int32_t a = SID_WAVE_DAC_6581[w0], b2 = SID_WAVE_DAC_6581[w1];
#else
                int32_t a = w0, b2 = w1;
#endif
                s = ((a + b2) / 2 - voice[v].wave_zero()) *
                    (int32_t)SID_ENV_DAC_6581[e];
#else
                s = voice[v].tick(rates, sync_reset, ring_flip);
#endif
#if CHIP_RIG_FILTERED > 0 && CHIP_RIG_BUSES > 0
                if (v < CHIP_RIG_FILTERED) bus_acc[v % CHIP_RIG_BUSES][i] += s;
                else                       dry[i] += s;
#else
                dry[i] += s;
#endif
            }
        }

#if CHIP_RIG_FILTERED > 0 && CHIP_RIG_BUSES > 0
        for (uint32_t b = 0; b < CHIP_RIG_BUSES; b++) {
            int16_t f = sid_filter_f_half(SID_MODEL_6581, (uint16_t)(600 + b * 300));
            int32_t q = sid_filter_q(SID_MODEL_6581, (uint8_t)(b * 4));
            for (uint32_t i = 0; i < n; i++) {
                dry[i] += bus[b].tick(bus_acc[b][i], f, q, SID_FILT_LP, CHIP_RIG_SAT != 0);
            }
        }
#endif
    }
};
