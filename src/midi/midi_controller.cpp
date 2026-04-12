#include "midi_controller.h"
#include "midi_parser.h"
#include "../voice_alloc.h"
#include "../osc/common.h"
#include "../osc/sample.h"
#include "../samples.h"
#include <cmath>

// --- Parser state ---
static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// --- Voice presets per MIDI channel (0–2 → buttons A/B/C, others → default) ---
struct MidiVoicePreset {
    Waveform waveform;
    uint16_t duty_cycle;
    float lfo_rate;
    float lfo_depth;
    float lfo_pitch_depth;
    float lfo_pwm_depth;
    FilterMode filter_mode;
    uint16_t filter_cutoff;
    uint16_t filter_resonance;
    int16_t filter_env_amount;
    float lfo_filter_depth;
    const SampleDef *sample;
};

static const MidiVoicePreset midi_presets[] = {
    // Ch 1: Fairlight sample, LP filter
    { WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  400, 16000, 8000, 0.0f,    &sararr1_sample },
    // Ch 2: Square BLEP, PWM LFO
    { WAVE_SQUARE_BLEP, 512, 3.0f, 0.0f, 0.0f, 0.5f, FILTER_LP,  800, 20000, 4000, 0.0f,    nullptr },
    // Ch 3: Saw BLEP, filter LFO
    { WAVE_SAW_BLEP,    512, 2.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  200, 24000, 0,    2000.0f, nullptr },
};

static constexpr uint8_t NUM_PRESETS = sizeof(midi_presets) / sizeof(midi_presets[0]);

static void midi_voice_on(VoiceParams &vp, uint8_t note, uint8_t velocity, uint8_t channel) {
    const MidiVoicePreset &pr = midi_presets[channel < NUM_PRESETS ? channel : NUM_PRESETS - 1];
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    vp.phase_inc = (pr.waveform == WAVE_SAMPLE && pr.sample)
        ? osc_sample_phase_inc(pr.sample, freq)
        : osc_phase_inc(freq);
    vp.amplitude = (int16_t)(velocity * 258);
    vp.waveform = pr.waveform;
    vp.duty_cycle = pr.duty_cycle;
    vp.lfo_rate = pr.lfo_rate;
    vp.lfo_depth = pr.lfo_depth;
    vp.lfo_pitch_depth = pr.lfo_pitch_depth;
    vp.lfo_pwm_depth = pr.lfo_pwm_depth;
    vp.filter_mode = pr.filter_mode;
    vp.filter_cutoff = pr.filter_cutoff;
    vp.filter_resonance = pr.filter_resonance;
    vp.filter_env_amount = pr.filter_env_amount;
    vp.lfo_filter_depth = pr.lfo_filter_depth;
    vp.sample = pr.sample;
    vp.trigger++;
    vp.gate = true;
}

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    if (len == 0) return;

    bool changed = false;
    VoiceParamBlock &shadow = params->shadow();
    shadow = params->active();

    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!midi_parser.feed(data[i], ev)) continue;

        if (ev.type == MIDI_NOTE_ON) {
            if (midi_note_voice[ev.note] >= 0) {
                shadow.voices[midi_note_voice[ev.note]].gate = false;
                voice_alloc_release(midi_note_voice[ev.note]);
            }
            int v = voice_alloc_allocate();
            if (v >= 0) {
                midi_note_voice[ev.note] = (int8_t)v;
                midi_voice_on(shadow.voices[v], ev.note, ev.velocity, ev.channel);
                changed = true;
            }
        } else if (ev.type == MIDI_NOTE_OFF) {
            int8_t v = midi_note_voice[ev.note];
            if (v >= 0) {
                shadow.voices[v].gate = false;
                voice_alloc_release(v);
                midi_note_voice[ev.note] = -1;
                changed = true;
            }
        }
    }

    if (changed) {
        params->commit();
    }
}
