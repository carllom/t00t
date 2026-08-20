#pragma once

#include "../input_layer.h"
#include "../voice_alloc.h"
#include <cmath>
#include <cstdint>

// Generic, module-agnostic glue between a parsed MidiEvent and the Router
// (src/input_layer.h's input_dispatch()): turns a raw MIDI message into a
// Shaped Input event and dispatches it against a module's own mapping
// table. Every module's own midi_controller.cpp composes these building
// blocks in whatever order/combination fits its own input model -- table
// contents and Handlers stay fully module-owned, nothing here assumes a
// particular routing shape beyond "MIDI byte in, Input event out."

// Program Change carries no MIDI CC number of its own; every module using
// it as a Configuration input shares this synthetic id, so the convention
// (Program Change -> CONFIGURATION) means the same thing everywhere.
static constexpr uint8_t MIDI_CONFIG_ID_PROGRAM = 0;

// Pitch bend likewise has no CC number; shares one synthetic id with the
// same reasoning, scoped under Modifier instead.
static constexpr uint8_t MIDI_MOD_ID_PITCH_BEND = 128;

// A 14-bit pitch bend value -> a phase_inc multiplier, +/-2 semitones.
inline float midi_bend_to_ratio(uint16_t bend14) {
    static constexpr uint16_t CENTER = 8192;
    static constexpr float RANGE_SEMITONES = 2.0f;
    float semitones = ((float)bend14 - (float)CENTER) / (float)CENTER * RANGE_SEMITONES;
    return powf(2.0f, semitones / 12.0f);
}

// Bank-select state (CC0 MSB / CC32 LSB): standard, module-agnostic MIDI
// semantics, stored per-channel regardless of which module is compiled in.
// Bypasses the Router entirely, same as it always has -- neither value
// needs channel/velocity matching. A module's own Program Change Handler
// reads these directly if it wants to combine bank + program number into a
// preset selection; midi_dispatch_program_change() below never touches
// this state itself, only the raw program number.
void midi_bank_select_init();
void midi_bank_select_msb(uint8_t channel, uint8_t value);
void midi_bank_select_lsb(uint8_t channel, uint8_t value);
uint8_t midi_channel_bank_msb(uint8_t channel);
uint8_t midi_channel_bank_lsb(uint8_t channel);

// Generic CC -> MODIFIER dispatch: passes the raw 0-127 byte through as
// `value.scalar` unconverted -- each Handler does its own source-native ->
// module-native scaling (unipolar, bipolar-centered, or none at all for a
// setter that does its own byte math), the same way FX setters already own
// their conversion.
template <typename Context, uint32_t N>
inline void midi_dispatch_cc(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                              uint8_t channel, uint8_t cc, uint8_t raw) {
    InputValue value{};
    value.channel = channel;
    value.scalar = (float)raw;
    input_dispatch(ctx, table, InputCategory::MODIFIER, cc, value);
}

template <typename Context, uint32_t N>
inline void midi_dispatch_pitch_bend(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                                      uint8_t channel, uint16_t bend14) {
    InputValue value{};
    value.channel = channel;
    value.scalar = midi_bend_to_ratio(bend14);
    input_dispatch(ctx, table, InputCategory::MODIFIER, MIDI_MOD_ID_PITCH_BEND, value);
}

// Generic Program Change -> CONFIGURATION dispatch: carries only the raw
// 0-127 program number.
template <typename Context, uint32_t N>
inline void midi_dispatch_program_change(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                                          uint8_t channel, uint8_t program) {
    InputValue value{};
    value.channel = channel;
    value.index = program;
    input_dispatch(ctx, table, InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, value);
}

// Generic NOTE dispatch with an already-resolved voice -- for a module
// whose voice for a given note doesn't come from the dynamic allocator
// (e.g. a fixed/predetermined voice per channel).
template <typename Context, uint32_t N>
inline void midi_dispatch_note(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                                uint8_t note, uint8_t channel, uint8_t velocity,
                                int8_t voice, bool note_on) {
    InputValue value{};
    value.channel = channel;
    value.note = note;
    value.velocity = velocity;
    value.voice = voice;
    value.note_on = note_on;
    input_dispatch(ctx, table, InputCategory::NOTE, note, value);
}

// Standard dynamic-voice-allocation NOTE_ON: steals any voice already
// playing this note (retrigger), allocates a fresh one, and dispatches.
// `midi_note_voice`/`voice_held` are the caller's own per-note/per-voice
// tracking arrays (module-owned, sized by that module's own MAX_VOICES).
// Returns the allocated voice, or -1 if none was available (nothing
// dispatched in that case).
template <typename Context, uint32_t N, uint32_t MaxVoices>
inline int8_t midi_dispatch_note_on_allocated(
        Context &ctx, const InputMapEntryT<Context> (&table)[N],
        uint8_t note, uint8_t channel, uint8_t velocity,
        int8_t (&midi_note_voice)[128], bool (&voice_held)[MaxVoices]) {
    if (midi_note_voice[note] >= 0) {
        int8_t old = midi_note_voice[note];
        ctx.voices[old].gate = false;
        voice_held[old] = false;
        voice_alloc_release(old);
    }
    int v = voice_alloc_allocate();
    if (v < 0) return -1;
    midi_note_voice[note] = (int8_t)v;
    voice_held[v] = true;
    midi_dispatch_note(ctx, table, note, channel, velocity, (int8_t)v, true);
    return (int8_t)v;
}

template <typename Context, uint32_t N, uint32_t MaxVoices>
inline void midi_dispatch_note_off_allocated(
        Context &ctx, const InputMapEntryT<Context> (&table)[N],
        uint8_t note, uint8_t channel,
        int8_t (&midi_note_voice)[128], bool (&voice_held)[MaxVoices]) {
    int8_t v = midi_note_voice[note];
    if (v < 0) return;
    midi_dispatch_note(ctx, table, note, channel, 0, v, false);
    voice_held[v] = false;
    voice_alloc_release(v);
    midi_note_voice[note] = -1;
}
