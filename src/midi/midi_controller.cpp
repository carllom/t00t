#include "midi_controller.h"
#include "midi_parser.h"
#include "../voice_alloc.h"
#include "../presets.h"
#include "../osc/common.h"
#include "../osc/sample.h"
#include <cmath>

// --- Parser state ---
static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// MIDI channel → preset index mapping
static const uint8_t channel_preset[] = {
    PRESET_FAIRLIGHT,   // Ch 1
    PRESET_SQUARE_PWM,  // Ch 2
    PRESET_SAW_FILTER,  // Ch 3
};
static constexpr uint8_t NUM_CHANNEL_PRESETS = sizeof(channel_preset) / sizeof(channel_preset[0]);

static void midi_voice_on(VoiceParams &vp, uint8_t note, uint8_t velocity, uint8_t channel) {
    uint8_t idx = channel < NUM_CHANNEL_PRESETS ? channel_preset[channel] : channel_preset[NUM_CHANNEL_PRESETS - 1];
    const VoicePreset &pr = presets[idx];
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    vp.phase_inc = (pr.waveform == WAVE_SAMPLE && pr.sample)
        ? osc_sample_phase_inc(pr.sample, freq)
        : osc_phase_inc(freq);
    voice_apply_preset(vp, pr);
    vp.amplitude = (int16_t)(velocity * 258);  // override with velocity
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
