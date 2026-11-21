#pragma once

#include "audio_common.h"
#include "multi_cooked.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "fx/phaser.h"
#include "fx/flanger.h"
#include "fx/chorus.h"
#include "fx/bitcrusher.h"
#include "fx/overdrive.h"
#include "pan.h"
#include <arm_acle.h>
#include <cstdint>

// The Multi's output stage on Core 1 (docs/platform.md "Mix"): the stereo
// sums the Parts' voices mix into, the shared FX on the share the Parts
// send to it, the master level, and the interleaved int16 output. Each
// engine's render owns one, and per buffer calls begin(), then part_gains()
// for each sounding voice and adds the voice into dry_l/dry_r (and its
// unsent share into cut_l/cut_r), then finish().
//
// A mono module mixes into dry_l (and cut_l) only, takes its gains from
// part_gains_mono(), and ends with finish_mono(), which leaves the int16
// conversion to the module's own output stage.

// A voice's stereo gains (Q15) from its Part's pan and volume, and the
// share of it not sent to the FX (Q15, 0 = all sent).
struct PartGains {
    int32_t l;
    int32_t r;
    int32_t cut_q15;
};

struct MultiOut {
    // Stereo dry mix, the share of it the Parts don't send to the FX (only
    // valid in a buffer where `any_cut`), and the mono FX send/return.
    int32_t dry_l[SAMPLES_PER_BUFFER];
    int32_t dry_r[SAMPLES_PER_BUFFER];
    int32_t cut_l[SAMPLES_PER_BUFFER];
    int32_t cut_r[SAMPLES_PER_BUFFER];
    int32_t fx_buf[SAMPLES_PER_BUFFER];
    bool any_cut;

    FxDelay   fx_delay;
    FxReverb  fx_reverb;
    FxPhaser  fx_phaser;
    FxFlanger fx_flanger;
    FxChorus  fx_chorus;
    FxBitcrusher fx_bitcrusher;
    FxOverdrive  fx_overdrive;
    uint8_t last_fx_type;  // a type switch clears the new effect's buffer

    void init() {
        fx_delay.init();
        fx_reverb.init();
        fx_phaser.init();
        fx_flanger.init();
        fx_chorus.init();
        fx_bitcrusher.init();
        fx_overdrive.init();
        last_fx_type = 0xFF;
        any_cut = false;
    }

    // Start a buffer: clear the dry mix.
    void begin() {
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            dry_l[i] = 0;
            dry_r[i] = 0;
        }
        any_cut = false;
    }

    // The gains of a voice on a Part mixed by `mix`. At unity volume and
    // full send they are the pan gains exactly and nothing is cut. The
    // first voice in a buffer with a cut clears cut_l/cut_r.
    PartGains part_gains(const PartMixCooked &mix) {
        PartGains g;
        pan_gains_q15((int16_t)mix.pan_q15, g.l, g.r);
        if (mix.volume_q15 != 32768) {
            g.l = (g.l * mix.volume_q15) >> 15;
            g.r = (g.r * mix.volume_q15) >> 15;
        }
        g.cut_q15 = 32768 - mix.send_q15;
        if (g.cut_q15 && !any_cut) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                cut_l[i] = 0;
                cut_r[i] = 0;
            }
            any_cut = true;
        }
        return g;
    }

    // A mono module's voice gains: its Part's volume in `l` and `r` (pan
    // doesn't apply), and the share not sent to the FX. The first voice in
    // a buffer with a cut clears cut_l.
    PartGains part_gains_mono(const PartMixCooked &mix) {
        PartGains g;
        g.l = g.r = mix.volume_q15;
        g.cut_q15 = 32768 - mix.send_q15;
        if (g.cut_q15 && !any_cut) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) cut_l[i] = 0;
            any_cut = true;
        }
        return g;
    }

    // End a buffer: run the FX and master on the mix and write `out`
    // (SAMPLES_PER_BUFFER interleaved stereo int16 frames). Mono send /
    // stereo return: the sent share of the dry mix is downmixed to mono,
    // run through the effect, and its wet output added to both channels.
    void finish(const EffectParams &fx, int32_t master_q15, int16_t *out) {
        const bool has_fx = (fx.type == FX_DELAY   || fx.type == FX_REVERB ||
                             fx.type == FX_PHASER  || fx.type == FX_FLANGER ||
                             fx.type == FX_CHORUS  || fx.type == FX_BITCRUSHER ||
                             fx.type == FX_OVERDRIVE);
        if (fx.type != last_fx_type) {
            if (fx.type == FX_DELAY)        fx_delay.init();
            else if (fx.type == FX_REVERB)  fx_reverb.init();
            else if (fx.type == FX_PHASER)  fx_phaser.init();
            else if (fx.type == FX_FLANGER) fx_flanger.init();
            else if (fx.type == FX_CHORUS)  fx_chorus.init();
            else if (fx.type == FX_BITCRUSHER) fx_bitcrusher.init();
            else if (fx.type == FX_OVERDRIVE)  fx_overdrive.init();
            last_fx_type = fx.type;
        }
        if (has_fx) {
            // The sent share: the dry mix less what the Parts don't send.
            if (any_cut) {
                for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                    dry_l[i] -= cut_l[i];
                    dry_r[i] -= cut_r[i];
                }
            }
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                fx_buf[i] = (dry_l[i] + dry_r[i]) >> 1;
            }
            if (fx.type == FX_DELAY)        fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else if (fx.type == FX_REVERB)  fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else if (fx.type == FX_PHASER)  fx_phaser.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else if (fx.type == FX_FLANGER) fx_flanger.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else if (fx.type == FX_CHORUS)  fx_chorus.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else if (fx.type == FX_BITCRUSHER) fx_bitcrusher.process(fx_buf, SAMPLES_PER_BUFFER, fx);
            else                                fx_overdrive.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        }

        // Crossfade the sent share dry/wet by the mix knob (Q15) rather than
        // summing full dry underneath -- fx_buf already carries mix as its
        // own gain, so FX MIX 127 is wet-only; dry plus near-unity wet would
        // clip at full mix. The unsent share stays dry at unity.
        const int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)fx.mix) * 258 : 32768;
        const bool add_cut = has_fx && any_cut;
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t l, r;
            if (has_fx) {
                l = (int32_t)(((int64_t)dry_scale * dry_l[i]) >> 15) + fx_buf[i];
                r = (int32_t)(((int64_t)dry_scale * dry_r[i]) >> 15) + fx_buf[i];
                if (add_cut) {
                    l += cut_l[i];
                    r += cut_r[i];
                }
            } else {
                l = dry_l[i];
                r = dry_r[i];
            }
            if (master_q15 != 32768) {
                l = (int32_t)(((int64_t)l * master_q15) >> 15);
                r = (int32_t)(((int64_t)r * master_q15) >> 15);
            }
            *out++ = (int16_t)__ssat(l, 16);
            *out++ = (int16_t)__ssat(r, 16);
        }
    }

    // End a mono buffer: run the FX and master on dry_l (less cut_l, the
    // unsent share) as finish() does, and write the mix, still at the
    // voices' own scale, into `out` (SAMPLES_PER_BUFFER values).
    void finish_mono(const EffectParams &fx, int32_t master_q15, int32_t *out) {
        const bool has_fx = select_fx(fx);
        if (has_fx) {
            if (any_cut) {
                for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) dry_l[i] -= cut_l[i];
            }
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) fx_buf[i] = dry_l[i];
            process_fx(fx);
        }
        const int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)fx.mix) * 258 : 32768;
        const bool add_cut = has_fx && any_cut;
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t m = has_fx ? (int32_t)(((int64_t)dry_scale * dry_l[i]) >> 15) + fx_buf[i] : dry_l[i];
            if (add_cut) m += cut_l[i];
            if (master_q15 != 32768) m = (int32_t)(((int64_t)m * master_q15) >> 15);
            out[i] = m;
        }
    }

private:
    // finish_mono()'s FX selection and processing. finish() keeps its own
    // copy: routing it through these changes how the stereo engines' render
    // compiles, and their CPU load was measured on finish() as it is.

    // Whether `fx` selects an effect; a type switch clears the newly
    // selected effect's buffer so a stale tail can't resurface.
    bool select_fx(const EffectParams &fx) {
        if (fx.type != last_fx_type) {
            if (fx.type == FX_DELAY)        fx_delay.init();
            else if (fx.type == FX_REVERB)  fx_reverb.init();
            else if (fx.type == FX_PHASER)  fx_phaser.init();
            else if (fx.type == FX_FLANGER) fx_flanger.init();
            else if (fx.type == FX_CHORUS)  fx_chorus.init();
            else if (fx.type == FX_BITCRUSHER) fx_bitcrusher.init();
            else if (fx.type == FX_OVERDRIVE)  fx_overdrive.init();
            last_fx_type = fx.type;
        }
        return fx.type == FX_DELAY   || fx.type == FX_REVERB ||
               fx.type == FX_PHASER  || fx.type == FX_FLANGER ||
               fx.type == FX_CHORUS  || fx.type == FX_BITCRUSHER ||
               fx.type == FX_OVERDRIVE;
    }

    // Run the selected effect on fx_buf in place.
    void process_fx(const EffectParams &fx) {
        if (fx.type == FX_DELAY)        fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_REVERB)  fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_PHASER)  fx_phaser.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_FLANGER) fx_flanger.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_CHORUS)  fx_chorus.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_BITCRUSHER) fx_bitcrusher.process(fx_buf, SAMPLES_PER_BUFFER, fx);
        else                                fx_overdrive.process(fx_buf, SAMPLES_PER_BUFFER, fx);
    }
};
