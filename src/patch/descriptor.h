#pragma once

#include "midi/nrpn.h"
#include <cstdint>
#include <cstdio>

// Descriptor vocabulary of the patch platform (CONTEXT.md "Patch platform",
// docs/platform.md). A module describes each of its native parameters with
// one ParamDescriptor; its descriptor table drives storage, MIDI mapping,
// the device UI and the editor. Read only by Core 0, the UI and SysEx --
// never by the render loop.

// The numeric values of ParamClass, ParamUnit and ParamCurve go out in the
// SysEx descriptor dump, so they are permanent: append only.

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
    BITS,      // a set of flags: bit n is named by label n, the value is their OR
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
    const char *const *labels = nullptr;  // ENUM: one per value from min, or none; BITS: one per bit from bit 0
    uint8_t label_count = 0;
};

// Stored width in bytes: values are stored as (value - min), so the range,
// not the sign, decides. A parameter's width must never change once
// patches have been stored with it.
constexpr uint32_t param_stored_width(const ParamDescriptor &d) {
    return (uint32_t)(d.max - d.min) <= 0xFFu ? 1u : 2u;
}

// A 14-bit controller value (0..16383) scaled linearly onto the parameter's
// range, rounded to nearest: 0 -> min, 16383 -> max.
constexpr int32_t param_value_from_14bit(const ParamDescriptor &d, uint32_t value14) {
    const int64_t range = (int64_t)d.max - (int64_t)d.min;
    return d.min + (int32_t)(((int64_t)value14 * range + 8191) / 16383);
}

// A 7-bit controller value (0..127), replicated to 14 bits so 127 -> max.
constexpr int32_t param_value_from_7bit(const ParamDescriptor &d, uint32_t value7) {
    return param_value_from_14bit(d, (value7 << 7) | value7);
}

// Controllers no parameter may claim as its default CC: Bank Select
// (0/32), Data Entry and NRPN/RPN select (6/38/98-101), and the Channel
// Mode messages (120-127).
constexpr bool cc_reserved(uint8_t cc) {
    return cc == 0 || cc == 32 || NrpnDecoder::is_nrpn_cc(cc) || cc >= 120;
}

// Index of the parameter in `table` whose default CC is `cc`, or N if none.
template <uint32_t N>
constexpr uint32_t descriptor_find_cc(const ParamDescriptor (&table)[N], uint8_t cc) {
    for (uint32_t i = 0; i < N; i++) {
        if (table[i].default_cc == cc) return i;
    }
    return N;
}

template <uint32_t N>
constexpr bool descriptor_uses_cc(const ParamDescriptor (&table)[N], uint8_t cc) {
    return descriptor_find_cc(table, cc) < N;
}

// Compile-time sanity check for a module's table, meant for a static_assert:
// IDs equal their index, defaults lie within range, ranges fit 16 bits,
// labels match the range (an ENUM's one per value, a BITS parameter's one
// per bit of 0..max), default CCs are unreserved and unique.
template <uint32_t N>
constexpr bool descriptor_table_valid(const ParamDescriptor (&table)[N]) {
    for (uint32_t i = 0; i < N; i++) {
        const ParamDescriptor &d = table[i];
        if (d.id != i) return false;
        if (d.min > d.max || d.def < d.min || d.def > d.max) return false;
        if ((int64_t)d.max - (int64_t)d.min > 0xFFFF) return false;
        if (d.unit == ParamUnit::BITS) {
            if (!d.labels || d.label_count < 1 || d.label_count > 16) return false;
            if (d.min != 0 || d.max != (int32_t)((1u << d.label_count) - 1)) return false;
        } else if (d.labels && (d.unit != ParamUnit::ENUM || d.label_count != d.max - d.min + 1)) {
            return false;
        }
        if (d.default_cc == kNoDefaultCc) continue;
        if (d.default_cc > 127 || cc_reserved(d.default_cc)) return false;
        if (descriptor_find_cc(table, d.default_cc) != i) return false;  // claimed twice
    }
    return true;
}

// One step of a value control (an encoder detent) from `value` in
// direction `dir` (+1 / -1), clamped to the range: by 1 for an ENUM, a
// BITS parameter (through every combination) or a range of at most 127, by about 6% of the distance above min for an
// EXPONENTIAL parameter, else by 1/128 of the range -- always at least 1.
constexpr int32_t param_step(const ParamDescriptor &d, int32_t value, int dir) {
    const int64_t range = (int64_t)d.max - (int64_t)d.min;
    const int64_t off = (int64_t)value - (int64_t)d.min;
    int64_t next = off;
    if (d.unit == ParamUnit::ENUM || d.unit == ParamUnit::BITS || range <= 127) {
        next = off + dir;
    } else if (d.curve == ParamCurve::EXPONENTIAL) {
        const int64_t scaled = dir > 0 ? off + (off * 6 + 50) / 100 : (off * 100 + 53) / 106;
        next = scaled == off ? off + dir : scaled;
    } else {
        const int64_t step = (range + 64) / 128;
        next = off + dir * (step < 1 ? 1 : step);
    }
    if (next < 0) next = 0;
    if (next > range) next = range;
    return d.min + (int32_t)next;
}

// `value` as display text into `out` (`n` bytes): an ENUM's label, a BITS
// parameter's set bits' labels joined by '+' ("-" for none), else the
// number in the parameter's unit ("1234Hz", "12.5k", "2.50Hz", "35.0%").
inline void param_format(const ParamDescriptor &d, int32_t value, char *out, uint32_t n) {
    const long v = value;
    const long a = v < 0 ? -v : v;
    const char *sign = v < 0 ? "-" : "";
    switch (d.unit) {
        case ParamUnit::ENUM:
            if (d.labels && value >= d.min && value <= d.max) {
                snprintf(out, n, "%s", d.labels[value - d.min]);
                return;
            }
            break;
        case ParamUnit::BITS: {
            if (!d.labels || n == 0) break;
            uint32_t at = 0;
            out[0] = '\0';
            for (uint32_t b = 0; b < d.label_count; b++) {
                if (!((uint32_t)value & (1u << b))) continue;
                const int w = snprintf(out + at, n - at, "%s%s", at ? "+" : "", d.labels[b]);
                if (w < 0 || at + (uint32_t)w >= n) return;  // truncated
                at += (uint32_t)w;
            }
            if (at == 0) snprintf(out, n, "-");
            return;
        }
        case ParamUnit::HZ:
            if (a < 10000) snprintf(out, n, "%ldHz", v);
            else snprintf(out, n, "%s%ld.%ldk", sign, a / 1000, (a % 1000) / 100);
            return;
        case ParamUnit::CENTI_HZ:
            snprintf(out, n, "%s%ld.%02ldHz", sign, a / 100, a % 100);
            return;
        case ParamUnit::PERMILLE:
            snprintf(out, n, "%s%ld.%ld%%", sign, a / 10, a % 10);
            return;
        case ParamUnit::NONE:
            break;
    }
    snprintf(out, n, "%ld", v);
}
