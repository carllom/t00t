#include "audio_engine.h"
#include "osc/oscillator.h"
#include "osc/sine.h"
#include "osc/noise.h"
#include "envelope.h"
#include "filter.h"
#include "ladder.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Groovebox render engine (Core 1). Per-voice VoiceType dispatch: each voice
// runs a focused inner loop for its instrument. The dispatch happens once per
// voice per buffer, not per sample. See groovebox.md.

// --- Telemetry for the Core 0 UI ---
static volatile uint8_t s_load_pct = 0;
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;
uint8_t audio_engine_load() { return s_load_pct; }

// --- Per-voice state — Core 1 only ---
static uint32_t voice_phase[MAX_VOICES];
static uint32_t voice_phase2[MAX_VOICES];   // snare tone 2
static uint16_t noise_lfsr[MAX_VOICES];
static uint8_t  last_trigger[MAX_VOICES];
static bool     voice_gated[MAX_VOICES];
static Envelope amp_env[MAX_VOICES];        // amplitude contour
static Envelope aux_env[MAX_VOICES];        // filter env (303) / pitch env (drums)
static SVFilter filter[MAX_VOICES];         // drums (BP/HP)
static LadderFilter ladder[MAX_VOICES];     // 303 (4-pole resonant LP)

static int32_t scratch[SAMPLES_PER_BUFFER];

// Post-mix effects (Core 1 only)
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

// Start an envelope as a one-shot decay: jump to full and decay to zero using
// the config's release_coeff (drums, 303 filter env). No attack — the instant
// onset is characteristic of analog drum machines.
static inline void env_oneshot(Envelope &e) {
    e.level = 1.0f;
    e.state = ENV_RELEASE;
}

// --- Per-type render paths. Each accumulates into scratch[]. -------------

// 303: saw/square through the 4-pole resonant ladder LP, amp ADSR, one-shot
// filter env that sweeps the cutoff down from base+env_mod to base.
static void render_303(uint32_t v, const VoiceParams &p) {
    uint32_t phase = voice_phase[v];
    float res = (float)p.filter_resonance * (1.0f / 32767.0f);   // 0..1
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        float amp_f = amp_env[v].advance(p.amp_env);
        if (amp_f <= 0.0f) break;
        float aux_f = aux_env[v].advance(p.aux_env);   // 1 -> 0
        int32_t level = (int32_t)(amp_f * 32767.0f);

        int32_t s = osc_sample(p.waveform, phase, p.duty_cycle, noise_lfsr[v], p.phase_inc);
        int32_t scaled = (s * p.amplitude) >> 15;
        scaled = (scaled * level) >> 15;

        float cutoff = (float)p.filter_cutoff + aux_f * (float)p.filter_env_amount;
        if (cutoff < 20.0f) cutoff = 20.0f;
        if (cutoff > 18000.0f) cutoff = 18000.0f;

        // Ladder works on normalized floats; scale Q15 <-> [-1, 1].
        float out = ladder[v].tick((float)scaled * (1.0f / 32768.0f), cutoff, res);
        scratch[i] += (int32_t)(out * 32768.0f);
        phase += p.phase_inc;
    }
    voice_phase[v] = phase;
}

// BD / tom: sine with a fast downward pitch sweep and an amp decay.
static void render_tonal_drum(uint32_t v, const VoiceParams &p) {
    uint32_t phase = voice_phase[v];
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        float amp_f = amp_env[v].advance(p.amp_env);
        if (amp_f <= 0.0f) break;
        float pit_f = aux_env[v].advance(p.aux_env);   // 1 -> 0
        int32_t level = (int32_t)(amp_f * 32767.0f);

        // Pitch env: eff_inc = base * (1 + depth * pit_f)
        int32_t depth = ((int32_t)p.pitch_env_depth * (int32_t)(pit_f * 32767.0f)) >> 15;
        uint32_t eff_inc = (uint32_t)((int32_t)p.phase_inc +
            (int32_t)(((int64_t)p.phase_inc * depth) >> 15));

        int32_t s = osc_sine(phase);
        int32_t scaled = (s * p.amplitude) >> 15;
        scaled = (scaled * level) >> 15;

        scratch[i] += scaled;
        phase += eff_inc;
    }
    voice_phase[v] = phase;
}

// Snare: two shell tones (pitch-swept) + noise through a band-pass, balanced by
// tone_level / noise_level.
static void render_snare(uint32_t v, const VoiceParams &p) {
    uint32_t ph1 = voice_phase[v];
    uint32_t ph2 = voice_phase2[v];
    int32_t q = svf_compute_q(p.filter_resonance);
    int16_t f = svf_compute_f_half(p.filter_cutoff);
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        float amp_f = amp_env[v].advance(p.amp_env);
        if (amp_f <= 0.0f) break;
        float pit_f = aux_env[v].advance(p.aux_env);
        int32_t level = (int32_t)(amp_f * 32767.0f);

        int32_t depth = ((int32_t)p.pitch_env_depth * (int32_t)(pit_f * 32767.0f)) >> 15;
        uint32_t inc1 = (uint32_t)((int32_t)p.phase_inc  + (int32_t)(((int64_t)p.phase_inc  * depth) >> 15));
        uint32_t inc2 = (uint32_t)((int32_t)p.phase_inc2 + (int32_t)(((int64_t)p.phase_inc2 * depth) >> 15));

        int32_t tone = (osc_sine(ph1) + osc_sine(ph2)) >> 1;
        int32_t noise = osc_noise(noise_lfsr[v]);
        noise = filter[v].tick(noise, f, q, p.filter_mode);   // FILTER_BP

        int32_t mix = ((tone * (int32_t)p.tone_level) >> 15)
                    + ((noise * (int32_t)p.noise_level) >> 15);
        int32_t scaled = (mix * p.amplitude) >> 15;
        scaled = (scaled * level) >> 15;

        scratch[i] += scaled;
        ph1 += inc1;
        ph2 += inc2;
    }
    voice_phase[v] = ph1;
    voice_phase2[v] = ph2;
}

// Hi-hat: noise through a high-pass with a short (closed) or long (open) decay.
static void render_noise_drum(uint32_t v, const VoiceParams &p) {
    int32_t q = svf_compute_q(p.filter_resonance);
    int16_t f = svf_compute_f_half(p.filter_cutoff);
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        float amp_f = amp_env[v].advance(p.amp_env);
        if (amp_f <= 0.0f) break;
        int32_t level = (int32_t)(amp_f * 32767.0f);

        int32_t s = osc_noise(noise_lfsr[v]);
        int32_t scaled = (s * p.amplitude) >> 15;
        scaled = (scaled * level) >> 15;
        scaled = filter[v].tick(scaled, f, q, p.filter_mode);   // FILTER_HP

        scratch[i] += scaled;
    }
}

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        voice_phase2[v] = 0;
        noise_lfsr[v] = 0xACE1u;
        last_trigger[v] = 0;
        voice_gated[v] = false;
        amp_env[v].init();
        aux_env[v].init();
        filter[v].init();
        ladder[v].init();
    }

    osc_init_sine();
    fx_delay.init();
    fx_reverb.init();

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        const VoiceParamBlock &vp = params->active();

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) scratch[i] = 0;

        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];
            if (p.type == VT_SILENT) continue;

            // New note?
            if (p.trigger != last_trigger[v]) {
                last_trigger[v] = p.trigger;
                voice_phase[v] = 0;
                voice_phase2[v] = 0;
                noise_lfsr[v] = 0xACE1u;
                filter[v].init();
                ladder[v].init();
                if (p.type == VT_TB303) amp_env[v].trigger();  // gated ADSR
                else                    env_oneshot(amp_env[v]);// one-shot decay
                env_oneshot(aux_env[v]);                        // filter/pitch env
                voice_gated[v] = true;
            }

            // Gate-off edge (303 only; drums are one-shot)
            if (!p.gate && voice_gated[v]) {
                if (p.type == VT_TB303) amp_env[v].release();
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }

            if (!amp_env[v].active()) continue;

            switch (p.type) {
                case VT_TB303:      render_303(v, p);         break;
                case VT_DRUM_BD:
                case VT_DRUM_TOM:   render_tonal_drum(v, p);  break;
                case VT_DRUM_SNARE: render_snare(v, p);       break;
                case VT_DRUM_HAT:   render_noise_drum(v, p);  break;
                default:            break;
            }
        }

        // Post-mix effect (delay / reverb), selected by CC74. Clear the newly
        // selected effect's buffer on a type switch so a stale tail can't leak.
        if (vp.fx.type != s_last_fx_type) {
            if (vp.fx.type == FX_DELAY)       fx_delay.init();
            else if (vp.fx.type == FX_REVERB) fx_reverb.init();
            s_last_fx_type = vp.fx.type;
        }
        if (vp.fx.type == FX_DELAY)       fx_delay.process(scratch, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_REVERB) fx_reverb.process(scratch, SAMPLES_PER_BUFFER, vp.fx);

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int16_t val = (int16_t)__ssat(scratch[i], 16);
            *out++ = val;  // left
            *out++ = val;  // right
        }

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        uint32_t bitmap = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            if (amp_env[v].active()) bitmap |= (1u << v);
        }
        multicore_fifo_push_timeout_us(bitmap, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
