#pragma once

#include <cstdint>

// Shared input vocabulary and generic dispatch mechanism between input
// sources (MIDI, VGA buttons, ...) and per-module setters. See #84/#85 and
// docs/engine.md's "MIDI Input" section for the design this implements.
//
// Deliberately shares only vocabulary (InputCategory) and a generic "match a
// table entry, call its setter" mechanism -- per-module mapping-table
// contents and setter logic stay exactly as forked as they are today (see
// #84's decision record and docs/logs/history_groovebox.md:618-771 for why).
//
// No event queue: input_dispatch() is a synchronous, direct call from parsed
// source event to per-module setter -- preserving docs/engine.md's "no
// intermediate event queue" invariant. Plain structs/templates only (no
// heap, no RTTI, no virtual dispatch), the same style as VoiceParamBlockT /
// ParamExchangeT below in spirit (see src/engine_base.h).

enum class InputCategory : uint8_t {
    NOTE,           // note number, velocity, implicit voice alloc/release
    STRIKE,         // generic discrete trigger, no note/pitch semantics
    MODIFIER,       // live continuous value, voice- or module-scoped
    CONFIGURATION,  // preset/template select, or an immutable character choice
    CLOCK,          // tempo pulse
    TRANSPORT,      // play/pause/stop -- distinct from the MIDI byte-carrier
                    // transport (USB/UART), see docs/engine.md
};

// Scope of a Modifier value: does it apply to one voice, or to every voice
// (module-global, e.g. a channel's currently-held voices)?
enum class ModifierScope : uint8_t { VOICE, GLOBAL };

// Normalized value carried by a dispatched input event. Which fields are
// meaningful depends on the InputCategory the event was matched under --
// setters know their own category and read only the field(s) that apply,
// the same convention midi_parser.h's MidiEvent already uses for data1/data2.
// Never carries raw MIDI bytes: callers normalize to native units (Hz,
// semitones, Q15, an enum index, ...) before building one of these.
struct InputValue {
    uint8_t channel = 0xFF;  // originating channel (0-15), 0xFF = not channel-scoped
    uint8_t note = 0;        // NOTE/STRIKE: note number (0-127)
    uint8_t velocity = 0;    // NOTE/STRIKE: velocity (0-127); dispatch may substitute this
    int8_t voice = -1;       // NOTE/STRIKE: resolved voice index, -1 if none
    bool note_on = false;    // NOTE/STRIKE: true = onset edge, false = release edge
    float scalar = 0.0f;     // MODIFIER: normalized native-unit value
    ModifierScope scope = ModifierScope::GLOBAL;  // MODIFIER: voice vs module-global
    uint8_t index = 0;       // CONFIGURATION: selected index/program
};

// One entry in a module's compile-time mapping table: an id range tagged
// with the InputCategory it drives and a pointer to the module's own
// setter. `id_low`/`id_high` are category-defined by the module (a MIDI CC
// number, a note range, a Clock/Transport sub-type, ...) -- the dispatcher
// only compares them against the event's own id, inclusive.
//
// `Context` is whatever a module's setters need to do their job (e.g. a
// VoiceParamBlock& for a per-voice synth engine) -- each module instantiates
// its own table type via this template, mirroring how engine_base.h's
// VoiceParamBlockT/ParamExchangeT are instantiated per module.
template <typename Context>
struct InputMapEntryT {
    InputCategory category;
    uint8_t id_low;
    uint8_t id_high;
    uint8_t channel_filter;  // 0xFF = any channel; else an exact channel is required
    uint8_t fixed_velocity;  // 0 = pass velocity through; else substitute this value
    void (*setter)(Context &ctx, const InputValue &value);
};

namespace input_layer_detail {

constexpr bool id_in_range(uint8_t id, uint8_t low, uint8_t high) {
    return id >= low && id <= high;
}

constexpr bool channel_allowed(uint8_t channel, uint8_t channel_filter) {
    return channel_filter == 0xFF || channel == channel_filter;
}

constexpr uint8_t apply_fixed_velocity(uint8_t velocity, uint8_t fixed_velocity) {
    return fixed_velocity != 0 ? fixed_velocity : velocity;
}

}  // namespace input_layer_detail

// The generic mutators named in #85: velocity fixed-value substitution,
// MIDI-channel filter, and (via id_low/id_high) note-range filter are the
// one place this layer does more than plumbing, because the behavior is
// genuinely identical across every module.
//
// Find the first entry in `table` matching (category, source_id, channel),
// apply its velocity substitution, and invoke its setter. No match: no-op,
// mirroring the `default: break` every per-module switch statement already
// has today.
template <typename Context, uint32_t N>
inline void input_dispatch(Context &ctx, const InputMapEntryT<Context> (&table)[N],
                            InputCategory category, uint8_t source_id, InputValue value) {
    using namespace input_layer_detail;
    for (uint32_t i = 0; i < N; i++) {
        const InputMapEntryT<Context> &entry = table[i];
        if (entry.category != category) continue;
        if (!id_in_range(source_id, entry.id_low, entry.id_high)) continue;
        if (!channel_allowed(value.channel, entry.channel_filter)) continue;
        value.velocity = apply_fixed_velocity(value.velocity, entry.fixed_velocity);
        entry.setter(ctx, value);
        return;
    }
}

// Compile-time capability check: every entry in `table` must carry a
// category present in `capabilities`. A module calls this in a
// static_assert against its own mapping table, so a table entry tagged
// with a category the module hasn't declared support for fails the build
// instead of silently working or silently being wrong.
template <typename Context, uint32_t N, uint32_t C>
constexpr bool input_table_declares_capabilities(const InputMapEntryT<Context> (&table)[N],
                                                  const InputCategory (&capabilities)[C]) {
    for (uint32_t i = 0; i < N; i++) {
        bool declared = false;
        for (uint32_t c = 0; c < C; c++) {
            if (table[i].category == capabilities[c]) {
                declared = true;
                break;
            }
        }
        if (!declared) return false;
    }
    return true;
}
