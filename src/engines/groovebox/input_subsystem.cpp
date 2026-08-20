#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "engine.h"
#include "kit.h"
#include "patterns.h"
#include "osc/common.h"
#include <cmath>

// groovebox's Input subsystem: the module-specific tail of the Input
// pipeline -- mapping table, Handlers, and this module's own routing
// decisions. MIDI bytes reach it via midi_controller_process(), built from
// midi_dispatch.h's shared, module-agnostic generic dispatch helpers
// (src/midi/midi_controller_generic.h), same as every other migrated
// module -- the mechanism is shared, the mapping table and Handlers are
// fully this module's own. Fixed instrument map, not a dynamic allocator:
//   - Every channel's Note On/Off dispatches uniformly as NOTE --
//     set_note() itself branches on value.channel to decide what a note
//     means:
//       - Channel 10 (index 9): drum machine -- note -> fixed voice via
//         the kit table (a one-shot trigger; note-off is a no-op).
//       - Any other channel: TB-303 -- monophonic acid bass on voice
//         GV_303, with this Handler's own legato/slide cross-note state.
//   - Channel 16 (index 15): pattern select. Program Change on that
//     channel dispatches as CONFIGURATION, exactly like every other
//     migrated module's preset selection -- channel_filter's exact-match
//     (not just "any channel") is what routes it to a dedicated Handler
//     instead of colliding with a real preset select.
//   - MIDI Clock drives the step sequencer directly from its own Handler,
//     without going through the Router a second time even though it plays
//     Note-like 303 steps internally.
//   - Pitch bend is 303-only; set_303_bend() ignores it on the drum
//     channel itself, since a bare channel_filter can't express "every
//     channel except one" (CONTEXT.md's Shaping entry).
//
// No dynamic voice_alloc anywhere in this file (fixed voice map), so no
// Voice Allocation Interface calls; bank-select isn't wired in either,
// since kit/preset switching beyond pattern select isn't implemented.

static constexpr uint8_t DRUM_CHANNEL    = 9;    // MIDI channel 10 (0-indexed)
static constexpr uint8_t PATTERN_CHANNEL = 15;   // MIDI channel 16 — pattern toggles
static constexpr uint8_t PULSES_PER_STEP = 6;    // 24 PPQN / 4 = 16th-note steps

// --- Arturia BeatStep Pro control map ------------------------------------
// The BSP's 16 encoders send CC 16..31 (absolute mode). Grid is 2x8: top row
// 16..23, bottom row 24..31. Encoders read raw 0..127. Reassign a control by
// changing its CC number here — nothing else references the raw numbers.
enum BspCC : uint8_t {
    CC_303_CUTOFF = 16,   // knob 1  — filter cutoff (live)
    CC_303_RESO   = 17,   // knob 2  — filter resonance (live)
    CC_303_ENVMOD = 18,   // knob 3  — filter env amount (next note)
    CC_303_DECAY  = 19,   // knob 4  — filter env decay (next note)
    CC_303_ACCENT = 20,   // knob 5  — accent depth (next note)
    CC_303_WAVE   = 21,   // knob 6  — saw <-> square (next note)
    CC_303_DRIVE  = 22,   // knob 7  — ladder overdrive / bite (next note)
    CC_FX_TYPE    = 24,   // knob 9  — effect select
    CC_FX_MIX     = 25,   // knob 10 — effect wet/dry
    CC_FX_P1      = 26,   // knob 11 — effect param 1
    CC_FX_P2      = 27,   // knob 12 — effect param 2
};

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
static float       g_303_accent = 1.0f; // accent depth scaler (CC), 0..1

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

static float note_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

// Accent: velocities above the threshold get a deeper filter-envelope sweep and
// extra level, ramping 0..1 across the top of the velocity range — the TB-303's
// signature accented-step emphasis.
static constexpr uint8_t ACCENT_VEL_THRESHOLD = 96;
static constexpr float   ACCENT_ENV_ADD  = 4000.0f;  // extra Hz of filter env at full accent
static constexpr float   ACCENT_AMP_BOOST = 0.4f;    // up to +40% level at full accent
static constexpr float   ACCENT_DRIVE_ADD = 2.0f;    // extra ladder overdrive at full accent
static constexpr float   MAX_303_DRIVE   = 6.0f;     // clamp base+accent drive

// Play a 303 note. slide = glide pitch from the previous note (legato);
// retrigger = restart phase + envelopes (a fresh, non-legato note). Called
// from set_note() (a real note on/off, non-drum channel) and
// seq_play_step() (a sequenced step) -- both funnel through here, not
// through the Router a second time.
static void play_303(VoiceParamBlock &shadow, uint8_t note, uint8_t velocity,
                     bool slide, bool retrigger) {
    VoiceParams &vp = shadow.voices[GV_303];
    uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq(note)) * g_303_bend);
    apply_303(vp, g_303, inc, velocity);

    float accent = 0.0f;
    if (velocity > ACCENT_VEL_THRESHOLD)
        accent = (float)(velocity - ACCENT_VEL_THRESHOLD) / (float)(127 - ACCENT_VEL_THRESHOLD);
    accent *= g_303_accent;   // CC-controlled accent depth (0 = flat, 1 = full)
    vp.filter_env_amount = (int16_t)((float)g_303.env_mod + accent * ACCENT_ENV_ADD);
    int32_t amp = (int32_t)((float)(velocity * 258) * (1.0f + accent * ACCENT_AMP_BOOST));
    vp.amplitude = (int16_t)(amp > 32767 ? 32767 : amp);

    // Accent also snarls the filter harder (on top of the base drive knob).
    float drive = g_303.drive + accent * ACCENT_DRIVE_ADD;
    vp.drive = drive > MAX_303_DRIVE ? MAX_303_DRIVE : drive;

    vp.slide = slide;
    vp.gate = true;
    if (retrigger) vp.trigger++;   // fresh note: Core 1 resets phase + envelopes
    g_303_note = note;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = 0;
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

// Configuration Handler: Program Change on the pattern channel selects a
// pattern -- pressing/selecting a new pattern switches to it; selecting
// the currently playing pattern stops. value.index carries the raw
// program number directly (no offset -- program 0 is pattern 0).
static void set_pattern_select(VoiceParamBlock &shadow, const InputValue &value) {
    uint8_t idx = value.index;
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

// Note Handler: value.channel decides what a note means -- the drum
// channel is a one-shot fixed-voice trigger via the kit table (not the
// dynamic allocator; note-off is a no-op), every other channel is the
// TB-303 on a fixed voice (GV_303), with its own legato/slide cross-note
// state (held_note[]/held_count), the same way other modules' NOTE
// Handlers own their retrigger-steal bookkeeping.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.channel == DRUM_CHANNEL) {
        if (!value.note_on) return;
        const KitInstrument *k = kit_find(kit_808, KIT_808_COUNT, value.note);
        if (!k) return;
        VoiceParams &vp = shadow.voices[k->voice];
        apply_kit(vp, *k, value.velocity);
        vp.trigger++;
        vp.gate = true;
        // Closed and open hi-hat share GV_HAT, so a closed hit re-triggers
        // the same voice and naturally cuts a ringing open hat — no
        // explicit choke needed.

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
        return;
    }

    if (value.note_on) {
        // A note played while another is held slides (legato); a fresh
        // note retriggers.
        bool legato = held_count > 0;
        held_push(value.note, value.velocity);
        play_303(shadow, value.note, value.velocity, legato, !legato);
    } else {
        held_remove(value.note);
        if (held_count == 0) {
            shadow.voices[GV_303].gate = false;   // last note released
            g_303_note = -1;
        } else {
            // Legato back to the most recent still-held note.
            uint8_t n = held_note[held_count - 1];
            uint8_t v = held_vel[held_count - 1];
            play_303(shadow, n, v, true, false);
        }
    }
}

// Modifier setters take the raw 0-127 CC byte via value.scalar and do
// their own source-native -> module-native conversion, the same pattern
// every migrated module's CC Handlers use.
static void set_303_cutoff(VoiceParamBlock &shadow, const InputValue &value) {  // exponential 100..10000 Hz, live
    float t = value.scalar * (1.0f / 127.0f);
    g_303.cutoff = (uint16_t)(100.0f * powf(100.0f, t));
    if (shadow.voices[GV_303].type == VT_TB303)
        shadow.voices[GV_303].filter_cutoff = g_303.cutoff;
    ui_state.mod = (uint8_t)value.scalar;
}

static void set_303_reso(VoiceParamBlock &shadow, const InputValue &value) {  // linear, capped for stability, live
    g_303.resonance = (uint16_t)(value.scalar * 237);  // max ~0.92
    if (shadow.voices[GV_303].type == VT_TB303)
        shadow.voices[GV_303].filter_resonance = g_303.resonance;
}

static void set_303_envmod(VoiceParamBlock &, const InputValue &value) {  // 0..8000 Hz, next note
    g_303.env_mod = (int16_t)(value.scalar * 63);
}

static void set_303_decay(VoiceParamBlock &, const InputValue &value) {  // exp 50..2500 ms, next note
    float t = value.scalar * (1.0f / 127.0f);
    g_303.env_decay_ms = (uint16_t)(50.0f * powf(50.0f, t));
}

static void set_303_accent(VoiceParamBlock &, const InputValue &value) {  // 0..1, next note
    g_303_accent = value.scalar * (1.0f / 127.0f);
}

static void set_303_wave(VoiceParamBlock &, const InputValue &value) {  // saw < 64 <= square, next note
    g_303.waveform = (value.scalar < 64.0f) ? WAVE_SAW : WAVE_SQUARE;
}

static void set_303_drive(VoiceParamBlock &, const InputValue &value) {  // 1.0..4.0, next note
    float t = value.scalar * (1.0f / 127.0f);
    g_303.drive = 1.0f + 3.0f * t;
}

// Pitch bend is 303-only; a bare channel_filter can't express "every
// channel except the drum channel" (CONTEXT.md's Shaping entry), so this
// Handler ignores it on the drum channel itself.
static void set_303_bend(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.channel == DRUM_CHANNEL) return;
    g_303_bend = value.scalar;
    if (g_303_note >= 0 && shadow.voices[GV_303].type == VT_TB303) {
        uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq((uint8_t)g_303_note)) * g_303_bend);
        shadow.voices[GV_303].phase_inc = inc;
    }
}

// FX setters write shadow.fx -- one instance per VoiceParamBlock, not
// per-voice -- a true module-global Modifier.
static void set_fx_type(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.type = (uint8_t)((uint32_t)value.scalar * FX_COUNT / 128u);
    ui_state.fx_type = shadow.fx.type;
}

static void set_fx_mix(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.mix = (uint8_t)value.scalar;
    ui_state.fx_mix = shadow.fx.mix;
}

static void set_fx_p1(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.p1 = (uint8_t)value.scalar;
    ui_state.fx_p1 = shadow.fx.p1;
}

static void set_fx_p2(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.p2 = (uint8_t)value.scalar;
    ui_state.fx_p2 = shadow.fx.p2;
}

// Clock Handler: drives the step sequencer directly, including playing (or
// releasing) a 303 step via play_303() -- never re-entering the Router for
// its own internal Note-like behavior. 24 PPQN; advances a 16th-note step
// every PULSES_PER_STEP pulses, playing on the step's first pulse.
static void set_clock(VoiceParamBlock &shadow, const InputValue &) {
    if (!(seq_playing && seq_clock_running)) return;
    if (seq_pulse == 0) seq_play_step(shadow);
    if (++seq_pulse >= PULSES_PER_STEP) {
        seq_pulse = 0;
        seq_step = (uint8_t)((seq_step + 1) % seq_patterns[seq_index].length);
    }
}

static void set_transport_start(VoiceParamBlock &, const InputValue &) {  // realign to the downbeat
    seq_step = 0;
    seq_pulse = 0;
    seq_clock_running = true;
    seq_prev_slide = false;
}

static void set_transport_continue(VoiceParamBlock &, const InputValue &) {
    seq_clock_running = true;
}

static void set_transport_stop(VoiceParamBlock &shadow, const InputValue &) {  // halt advancing and silence the 303
    seq_clock_running = false;
    shadow.voices[GV_303].gate = false;
    g_303_note = -1;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::CONFIGURATION,
    InputCategory::NOTE,
    InputCategory::MODIFIER,
    InputCategory::CLOCK,
    InputCategory::TRANSPORT,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low                  id_high                 channel           fixed_vel  setter
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, PATTERN_CHANNEL,   0,       set_pattern_select },
    { InputCategory::NOTE,          0,                      127,                    0xFF,              0,       set_note },
    { InputCategory::MODIFIER,      CC_303_CUTOFF,          CC_303_CUTOFF,          0xFF,              0,       set_303_cutoff },
    { InputCategory::MODIFIER,      CC_303_RESO,            CC_303_RESO,            0xFF,              0,       set_303_reso },
    { InputCategory::MODIFIER,      CC_303_ENVMOD,          CC_303_ENVMOD,          0xFF,              0,       set_303_envmod },
    { InputCategory::MODIFIER,      CC_303_DECAY,           CC_303_DECAY,           0xFF,              0,       set_303_decay },
    { InputCategory::MODIFIER,      CC_303_ACCENT,          CC_303_ACCENT,          0xFF,              0,       set_303_accent },
    { InputCategory::MODIFIER,      CC_303_WAVE,            CC_303_WAVE,            0xFF,              0,       set_303_wave },
    { InputCategory::MODIFIER,      CC_303_DRIVE,           CC_303_DRIVE,           0xFF,              0,       set_303_drive },
    { InputCategory::MODIFIER,      MIDI_MOD_ID_PITCH_BEND, MIDI_MOD_ID_PITCH_BEND, 0xFF,              0,       set_303_bend },
    { InputCategory::MODIFIER,      CC_FX_TYPE,             CC_FX_TYPE,             0xFF,              0,       set_fx_type },
    { InputCategory::MODIFIER,      CC_FX_MIX,              CC_FX_MIX,              0xFF,              0,       set_fx_mix },
    { InputCategory::MODIFIER,      CC_FX_P1,               CC_FX_P1,               0xFF,              0,       set_fx_p1 },
    { InputCategory::MODIFIER,      CC_FX_P2,               CC_FX_P2,               0xFF,              0,       set_fx_p2 },
    { InputCategory::CLOCK,         0,                      0,                      0xFF,              0,       set_clock },
    { InputCategory::TRANSPORT,     MIDI_START,             MIDI_START,             0xFF,              0,       set_transport_start },
    { InputCategory::TRANSPORT,     MIDI_CONTINUE,          MIDI_CONTINUE,          0xFF,              0,       set_transport_continue },
    { InputCategory::TRANSPORT,     MIDI_STOP,              MIDI_STOP,              0xFF,              0,       set_transport_stop },
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "groovebox mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    g_303 = tb303_default;
    g_303_note = -1;
    g_303_bend = 1.0f;
    g_303_accent = 1.0f;
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
    midi_controller_process_generic(data, len, params, kMappingTable);
}
