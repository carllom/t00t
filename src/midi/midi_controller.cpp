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

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Per-channel state (16 MIDI channels) ---
// Default MIDI channel → preset mapping (channels beyond the list fall back
// to the last entry). Program change overrides channel_program[] at runtime.
static const uint8_t channel_preset[] = {
    PRESET_FAIRLIGHT,   // Ch 1
    PRESET_SQUARE_PWM,  // Ch 2
    PRESET_SAW_FILTER,  // Ch 3
};
static constexpr uint8_t NUM_CHANNEL_PRESETS = sizeof(channel_preset) / sizeof(channel_preset[0]);

static constexpr uint8_t NUM_CHANNELS = 16;
static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float PITCH_BEND_RANGE_SEMITONES = 2.0f;

static uint8_t channel_program[NUM_CHANNELS];   // current preset index per channel
static float   channel_bend_ratio[NUM_CHANNELS]; // phase_inc multiplier (1.0 = centered)
static int16_t channel_mod[NUM_CHANNELS];        // mod-wheel vibrato depth, Q15
static uint8_t channel_bank_msb[NUM_CHANNELS];   // CC0  — stored for future use
static uint8_t channel_bank_lsb[NUM_CHANNELS];   // CC32 — stored for future use

static uint8_t default_preset_for_channel(uint8_t ch) {
    return ch < NUM_CHANNEL_PRESETS ? channel_preset[ch] : channel_preset[NUM_CHANNEL_PRESETS - 1];
}

// microKORG program numbering: the program-change value's tens digit is the row
// (0-7) and the ones digit is the column (0-7), so a bank holds programs
// 0-7, 10-17, ... 70-77 (64 total; columns 8/9 and rows >7 are unused). Bank
// select adds another 64. Returns a linear slot 0-127, or -1 if the value isn't
// a valid microKORG program.
static int microkorg_slot(uint8_t bank, uint8_t pc) {
    uint8_t row = pc / 10, col = pc % 10;
    if (row > 7 || col > 7) return -1;
    return (int)(bank & 1) * 64 + row * 8 + col;
}

// --- UI snapshot (updated on each event, read by the display) ---
static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) {
    *out = ui_state;
}

// Map a 14-bit pitch bend value to a phase_inc multiplier.
static float bend_to_ratio(uint16_t bend14) {
    float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER) / (float)PITCH_BEND_CENTER
                      * PITCH_BEND_RANGE_SEMITONES;
    return powf(2.0f, semitones / 12.0f);
}

static void midi_voice_on(VoiceParamBlock &shadow, int v, uint8_t note, uint8_t velocity, uint8_t channel) {
    const VoicePreset &pr = presets[channel_program[channel]];
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    uint32_t base = (pr.waveform == WAVE_SAMPLE && pr.sample)
        ? osc_sample_phase_inc(pr.sample, freq)
        : osc_phase_inc(freq);

    voice_base_inc[v] = base;
    voice_channel[v] = channel;
    voice_held[v] = true;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = channel;
    ui_state.program = channel_program[channel];

    VoiceParams &vp = shadow.voices[v];
    voice_apply_preset(vp, pr);
    vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[channel]);
    vp.amplitude = (int16_t)(velocity * 258);  // override with velocity
    vp.mod_depth = channel_mod[channel];
    vp.trigger++;
    vp.gate = true;
}

// Re-scale phase_inc for every held voice on a channel after a bend change.
static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].phase_inc =
                (uint32_t)((float)voice_base_inc[v] * channel_bend_ratio[channel]);
        }
    }
}

// Push mod-wheel depth to every held voice on a channel.
static void apply_channel_mod(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].mod_depth = channel_mod[channel];
        }
    }
}

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_program[ch] = default_preset_for_channel(ch);
        channel_bend_ratio[ch] = 1.0f;
        channel_mod[ch] = 0;
        channel_bank_msb[ch] = 0;
        channel_bank_lsb[ch] = 0;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = default_preset_for_channel(0);
    ui_state.bend = 0;
    ui_state.mod = 0;
    // Match ParamExchange::init() fx defaults (300 ms, ~0.4 fbk, dry).
    ui_state.fx_mix = 0;
    ui_state.fx_fbk = 55;
    ui_state.fx_delay_ms = 300;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    if (len == 0) return;

    bool changed = false;
    VoiceParamBlock &shadow = params->shadow();
    shadow = params->active();

    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!midi_parser.feed(data[i], ev)) continue;

        switch (ev.type) {
            case MIDI_NOTE_ON: {
                uint8_t note = ev.data1;
                if (midi_note_voice[note] >= 0) {
                    int8_t old = midi_note_voice[note];
                    shadow.voices[old].gate = false;
                    voice_held[old] = false;
                    voice_alloc_release(old);
                }
                int v = voice_alloc_allocate();
                if (v >= 0) {
                    midi_note_voice[note] = (int8_t)v;
                    midi_voice_on(shadow, v, note, ev.data2, ev.channel);
                    changed = true;
                }
                break;
            }
            case MIDI_NOTE_OFF: {
                int8_t v = midi_note_voice[ev.data1];
                if (v >= 0) {
                    shadow.voices[v].gate = false;
                    voice_held[v] = false;
                    voice_alloc_release(v);
                    midi_note_voice[ev.data1] = -1;
                    changed = true;
                }
                break;
            }
            case MIDI_CC: {
                switch (ev.data1) {
                    case 1:  // mod wheel → vibrato depth
                        channel_mod[ev.channel] = (int16_t)(ev.data2 * 258);  // 0..127 → ~0..32766
                        apply_channel_mod(shadow, ev.channel);
                        ui_state.mod = ev.data2;
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 73: {  // effect wet/dry mix (global)
                        shadow.fx.mix_q15 = (int16_t)(ev.data2 * 258);  // 0..~32766
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    }
                    case 72: {  // effect feedback (global) — capped below runaway
                        shadow.fx.feedback_q15 = (int16_t)(ev.data2 * 236);  // max ≈ 0.91
                        ui_state.fx_fbk = ev.data2;
                        changed = true;
                        break;
                    }
                    case 75: {  // effect delay time (global): 20..1000 ms
                        uint32_t ms = 20 + (uint32_t)ev.data2 * 980u / 127u;
                        shadow.fx.delay_samples = (uint16_t)(ms * SAMPLE_RATE / 1000u);
                        ui_state.fx_delay_ms = (uint16_t)ms;
                        changed = true;
                        break;
                    }
                    case 0:   channel_bank_msb[ev.channel] = ev.data2; break;
                    case 32:  channel_bank_lsb[ev.channel] = ev.data2; break;
                    default:  break;  // other CCs — to be mapped later
                }
                break;
            }
            case MIDI_PITCH_BEND: {
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                channel_bend_ratio[ev.channel] = bend_to_ratio(bend14);
                apply_channel_bend(shadow, ev.channel);
                ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                ui_state.last_channel = ev.channel;
                changed = true;
                break;
            }
            case MIDI_PROGRAM_CHANGE: {
                // microKORG numbering (row = tens digit, col = ones digit) plus
                // bank select. Affects future notes only. Bank comes from CC0
                // (MSB) — switch to channel_bank_lsb if the microKORG uses CC32.
                int slot = microkorg_slot(channel_bank_msb[ev.channel], ev.data1);
                if (slot >= 0) {
                    channel_program[ev.channel] = (uint8_t)(slot % PRESET_COUNT);
                    ui_state.program = channel_program[ev.channel];
                    ui_state.last_channel = ev.channel;
                }
                break;
            }
        }
    }

    if (changed) {
        params->commit();
    }
}
