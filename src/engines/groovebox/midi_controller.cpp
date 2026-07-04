#include "midi_controller.h"
#include "midi_parser.h"
#include "engine.h"
#include "kit.h"
#include "patterns.h"
#include "osc/common.h"
#include <cmath>

// Groovebox MIDI routing (Core 0). Replaces the subtractive engine's
// poly-allocator routing with a fixed instrument map:
//   - Channel 10 (index 9): drum machine — note -> fixed voice via the kit table.
//   - Any other channel:     TB-303 — monophonic acid bass on voice GV_303.
//
// Shares the transport-agnostic MidiParser and the midi_controller.h interface;
// the transports (usb/uart) call midi_controller_process() unchanged.

static constexpr uint8_t DRUM_CHANNEL    = 9;    // MIDI channel 10 (0-indexed)
static constexpr uint8_t PATTERN_CHANNEL = 15;   // MIDI channel 16 — pattern toggles
static constexpr uint8_t PULSES_PER_STEP = 6;    // 24 PPQN / 4 = 16th-note steps

static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float    PITCH_BEND_RANGE_SEMITONES = 2.0f;

static MidiParser midi_parser;

// --- Sequencer state (Core 0) ---
static bool    seq_playing = false;      // a pattern is armed/looping
static uint8_t seq_index = 0;            // which pattern
static uint8_t seq_step = 0;             // current step
static uint8_t seq_pulse = 0;            // clock pulses within the current step
static bool    seq_clock_running = true; // MIDI transport running (start/stop)
static bool    seq_prev_slide = false;   // previous step's slide flag (glide into this step)

// --- TB-303 live state ---
static Tb303Preset g_303;          // mutated live by CCs
static int         g_303_note = -1; // currently sounding 303 note (-1 = none)
static float       g_303_bend = 1.0f;

// Held-note stack for monophonic last-note priority + legato. Overlapping notes
// slide (portamento); releasing back to a still-held note slides to it.
static uint8_t held_note[16];
static uint8_t held_vel[16];
static uint8_t held_count = 0;

static void held_push(uint8_t note, uint8_t vel) {
    if (held_count < 16) { held_note[held_count] = note; held_vel[held_count] = vel; held_count++; }
}
static void held_remove(uint8_t note) {
    for (uint8_t i = 0; i < held_count; i++) {
        if (held_note[i] == note) {
            for (uint8_t j = i; j + 1 < held_count; j++) {
                held_note[j] = held_note[j + 1];
                held_vel[j]  = held_vel[j + 1];
            }
            held_count--;
            return;
        }
    }
}

// --- UI snapshot ---
static MidiUiState ui_state;
void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

static float note_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

// Accent: velocities above the threshold get a deeper filter-envelope sweep and
// extra level, ramping 0..1 across the top of the velocity range — the TB-303's
// signature accented-step emphasis.
static constexpr uint8_t ACCENT_VEL_THRESHOLD = 96;
static constexpr float   ACCENT_ENV_ADD  = 4000.0f;  // extra Hz of filter env at full accent
static constexpr float   ACCENT_AMP_BOOST = 0.4f;    // up to +40% level at full accent

// Play a 303 note. slide = glide pitch from the previous note (legato);
// retrigger = restart phase + envelopes (a fresh, non-legato note).
static void play_303(VoiceParamBlock &shadow, uint8_t note, uint8_t velocity,
                     bool slide, bool retrigger) {
    VoiceParams &vp = shadow.voices[GV_303];
    uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq(note)) * g_303_bend);
    apply_303(vp, g_303, inc, velocity);

    float accent = 0.0f;
    if (velocity > ACCENT_VEL_THRESHOLD)
        accent = (float)(velocity - ACCENT_VEL_THRESHOLD) / (float)(127 - ACCENT_VEL_THRESHOLD);
    vp.filter_env_amount = (int16_t)((float)g_303.env_mod + accent * ACCENT_ENV_ADD);
    int32_t amp = (int32_t)((float)(velocity * 258) * (1.0f + accent * ACCENT_AMP_BOOST));
    vp.amplitude = (int16_t)(amp > 32767 ? 32767 : amp);

    vp.slide = slide;
    vp.gate = true;
    if (retrigger) vp.trigger++;   // fresh note: Core 1 resets phase + envelopes
    g_303_note = note;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = 0;
}

static void trigger_drum(VoiceParamBlock &shadow, uint8_t note, uint8_t velocity) {
    const KitInstrument *k = kit_find(kit_808, KIT_808_COUNT, note);
    if (!k) return;
    VoiceParams &vp = shadow.voices[k->voice];
    apply_kit(vp, *k, velocity);
    vp.trigger++;
    vp.gate = true;
    // Closed and open hi-hat share GV_HAT, so a closed hit re-triggers the same
    // voice and naturally cuts a ringing open hat — no explicit choke needed.

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = DRUM_CHANNEL;
}

// --- Sequencer ------------------------------------------------------------

static void seq_stop(VoiceParamBlock &shadow) {
    seq_playing = false;
    seq_prev_slide = false;
    shadow.voices[GV_303].gate = false;
    g_303_note = -1;
}

// Play the current step's 303 event (or gate off on a rest). SEQ_SLIDE marks a
// note that glides *into the next* step (303 convention), so a step is played
// legato when the previous step was a slide.
static void seq_play_step(VoiceParamBlock &shadow) {
    const SeqPattern &pat = seq_patterns[seq_index];
    const SeqStep &st = pat.steps[seq_step % pat.length];
    if (st.note == 0) {
        shadow.voices[GV_303].gate = false;   // rest
        g_303_note = -1;
        seq_prev_slide = false;               // a rest breaks the slide chain
        return;
    }
    // Glide into this note if the previous step requested a slide (and there is
    // a note to glide from — can't slide out of a rest).
    bool slide = seq_prev_slide && (g_303_note >= 0);
    uint8_t vel = (st.flags & SEQ_ACCENT) ? 122 : 90;
    play_303(shadow, st.note, vel, slide, !slide);
    seq_prev_slide = (st.flags & SEQ_SLIDE) != 0;
}

// Key on the pattern channel toggles its pattern: pressing a new pattern's key
// switches to it; pressing the playing pattern's key stops.
static void seq_toggle(VoiceParamBlock &shadow, uint8_t note) {
    if (note < SEQ_PATTERN_BASE_NOTE) return;
    uint8_t idx = note - SEQ_PATTERN_BASE_NOTE;
    if (idx >= SEQ_PATTERN_COUNT) return;
    if (seq_playing && seq_index == idx) {
        seq_stop(shadow);
    } else {
        seq_index = idx;
        seq_playing = true;
        seq_step = 0;
        seq_pulse = 0;   // next clock pulse plays step 0
        seq_prev_slide = false;
    }
}

void midi_controller_init() {
    midi_parser.init();
    g_303 = tb303_default;
    g_303_note = -1;
    g_303_bend = 1.0f;
    held_count = 0;
    seq_playing = false;
    seq_index = 0;
    seq_step = 0;
    seq_pulse = 0;
    seq_clock_running = true;
    seq_prev_slide = false;

    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = 0;
    ui_state.bend = 0;
    ui_state.mod = 0;
    ui_state.fx_type = FX_DELAY;
    ui_state.fx_mix = 0;
    ui_state.fx_p1 = 55;
    ui_state.fx_p2 = 36;
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
            case MIDI_NOTE_ON:
                if (ev.channel == DRUM_CHANNEL) {
                    trigger_drum(shadow, ev.data1, ev.data2);
                } else if (ev.channel == PATTERN_CHANNEL) {
                    seq_toggle(shadow, ev.data1);
                } else {
                    // Mono 303: a note played while another is held slides
                    // (legato); a fresh note retriggers.
                    bool legato = held_count > 0;
                    held_push(ev.data1, ev.data2);
                    play_303(shadow, ev.data1, ev.data2, legato, !legato);
                }
                changed = true;
                break;

            case MIDI_NOTE_OFF:
                if (ev.channel != DRUM_CHANNEL && ev.channel != PATTERN_CHANNEL) {
                    held_remove(ev.data1);
                    if (held_count == 0) {
                        shadow.voices[GV_303].gate = false;   // last note released
                        g_303_note = -1;
                    } else {
                        // Legato back to the most recent still-held note.
                        uint8_t n = held_note[held_count - 1];
                        uint8_t v = held_vel[held_count - 1];
                        play_303(shadow, n, v, true, false);
                    }
                    changed = true;
                }
                // Drums are one-shot: note-off is ignored.
                break;

            case MIDI_CC:
                switch (ev.data1) {
                    case 1: {   // mod wheel -> 303 cutoff, exponential 100..10000 Hz
                        float t = (float)ev.data2 * (1.0f / 127.0f);
                        g_303.cutoff = (uint16_t)(100.0f * powf(100.0f, t));
                        if (shadow.voices[GV_303].type == VT_TB303)
                            shadow.voices[GV_303].filter_cutoff = g_303.cutoff;
                        ui_state.mod = ev.data2;
                        changed = true;
                        break;
                    }
                    case 71: {  // CC71 -> 303 resonance (linear, capped for stability)
                        g_303.resonance = (uint16_t)(ev.data2 * 237);  // max ~0.92
                        if (shadow.voices[GV_303].type == VT_TB303)
                            shadow.voices[GV_303].filter_resonance = g_303.resonance;
                        changed = true;
                        break;
                    }
                    case 74:  // effect type select
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case 73:  // effect wet/dry mix
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case 72:  // effect param 1
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case 75:  // effect param 2
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    default: break;
                }
                break;

            case MIDI_PITCH_BEND: {
                if (ev.channel != DRUM_CHANNEL) {
                    uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                    float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER)
                                      / (float)PITCH_BEND_CENTER * PITCH_BEND_RANGE_SEMITONES;
                    g_303_bend = powf(2.0f, semitones / 12.0f);
                    if (g_303_note >= 0 && shadow.voices[GV_303].type == VT_TB303) {
                        uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq((uint8_t)g_303_note)) * g_303_bend);
                        shadow.voices[GV_303].phase_inc = inc;
                    }
                    ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                    changed = true;
                }
                break;
            }

            case MIDI_PROGRAM_CHANGE:
                // Kit / preset switching — to be added with the 909 kit.
                break;

            case MIDI_CLOCK:
                // 24 PPQN. Advance a 16th-note step every 6 pulses; the step
                // plays on its first pulse.
                if (seq_playing && seq_clock_running) {
                    if (seq_pulse == 0) { seq_play_step(shadow); changed = true; }
                    if (++seq_pulse >= PULSES_PER_STEP) {
                        seq_pulse = 0;
                        seq_step = (uint8_t)((seq_step + 1) % seq_patterns[seq_index].length);
                    }
                }
                break;

            case MIDI_START:      // transport start — realign to the downbeat
                seq_step = 0;
                seq_pulse = 0;
                seq_clock_running = true;
                seq_prev_slide = false;
                break;

            case MIDI_CONTINUE:
                seq_clock_running = true;
                break;

            case MIDI_STOP:       // halt advancing and silence the 303
                seq_clock_running = false;
                shadow.voices[GV_303].gate = false;
                g_303_note = -1;
                changed = true;
                break;
        }
    }

    if (changed) params->commit();
}
