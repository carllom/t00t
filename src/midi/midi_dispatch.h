#pragma once

#include "../input_layer.h"
#include <cmath>
#include <cstdint>

// Generic, module-agnostic glue between a parsed MidiEvent and the Router
// (src/input_layer.h's input_dispatch()): turns a raw MIDI message into a
// Shaped Input event and dispatches it against a module's own mapping
// table. Every module's own MIDI routing file (src/engines/<name>/,
// input_subsystem.cpp once migrated) composes these building blocks in
// whatever order/combination fits its own input model -- table contents
// and Handlers stay fully module-owned, nothing here assumes a particular
// routing shape beyond "MIDI byte in, Input event out."

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

// Generic NOTE dispatch. Carries only the raw note/channel/velocity/edge --
// voice resolution (dynamic `voice_alloc`, a fixed/predetermined voice, or
// none) is never done here: it's the Voice Allocation Interface's job,
// reached from inside a module's own NOTE Handler (never interleaved in
// parsing/dispatch, see CONTEXT.md's "Voice Allocation Interface" entry).
// `voice` defaults to -1 (unresolved); a Handler that resolves its own
// voice reads `value.note` and does so itself.
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

// Generic Transport dispatch: play/pause/stop, no payload beyond which
// transport message it is (Transport is module-global, not per-channel).
// `transport_id` is the MIDI transport message type itself
// (MIDI_START/CONTINUE/STOP, midi_parser.h) -- already a small, distinct,
// meaningful value, reused directly as the Router's matching id rather
// than inventing a second numbering.
template <typename Context, uint32_t N>
inline void midi_dispatch_transport(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                                     uint8_t transport_id) {
    InputValue value{};
    input_dispatch(ctx, table, InputCategory::TRANSPORT, transport_id, value);
}
