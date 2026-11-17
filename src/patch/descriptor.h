#pragma once

#include <cstdint>

// Descriptor vocabulary of the patch platform (CONTEXT.md "Patch platform",
// docs/platform.md). A module describes each of its native parameters with
// one ParamDescriptor; its descriptor table drives storage, MIDI mapping,
// the device UI and the editor. Read only by Core 0, the UI and SysEx --
// never by the render loop.

// How an edit to a parameter reaches sound.
enum class ParamClass : uint8_t {
    LIVE,           // applies to already-sounding notes
    NOTE_ON,        // fixed for a note when it starts
    DATA_EXCHANGE,  // opaque bulk table data, moved as a block
};

// What a parameter's integer value means, for display.
enum class ParamUnit : uint8_t {
    NONE,      // plain number
    ENUM,      // index into a module-defined list
    HZ,
    CENTI_HZ,  // Hz * 100
    PERMILLE,  // fraction * 1000
};

// How a control should map onto the value range.
enum class ParamCurve : uint8_t {
    LINEAR,
    EXPONENTIAL,
};

static constexpr uint8_t kNoDefaultCc = 0xFF;

struct ParamDescriptor {
    uint16_t id;          // permanent within the module; IDs are only appended, equal to table index
    const char *name;
    const char *group;    // UI group / Page
    int32_t min;
    int32_t max;
    int32_t def;          // value for patches stored before this parameter existed
    ParamUnit unit;
    ParamCurve curve;
    ParamClass cls;
    bool quick_edit;      // shown on the Quick-edit Page
    uint8_t default_cc;   // kNoDefaultCc = none
};

// Stored width in bytes: values are stored as (value - min), so the range,
// not the sign, decides. A parameter's width must never change once
// patches have been stored with it.
constexpr uint32_t param_stored_width(const ParamDescriptor &d) {
    return (uint32_t)(d.max - d.min) <= 0xFFu ? 1u : 2u;
}

// Compile-time sanity check for a module's table, meant for a static_assert:
// IDs equal their index, defaults lie within range, ranges fit 16 bits.
template <uint32_t N>
constexpr bool descriptor_table_valid(const ParamDescriptor (&table)[N]) {
    for (uint32_t i = 0; i < N; i++) {
        const ParamDescriptor &d = table[i];
        if (d.id != i) return false;
        if (d.min > d.max || d.def < d.min || d.def > d.max) return false;
        if ((int64_t)d.max - (int64_t)d.min > 0xFFFF) return false;
    }
    return true;
}
