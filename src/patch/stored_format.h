#pragma once

#include "descriptor.h"
#include "module_id.h"
#include <cstdint>

// The stored Patch format: engine-independent, versioned, little-endian.
//
//   offset  size  field
//   0       4     magic "T0PT"
//   4       1     module ID (ModuleId)
//   5       1     schema version
//   6       16    name, ASCII, NUL-padded (not necessarily terminated)
//   22      2     payload length in bytes
//   24      n     payload: one value per parameter in ID order, each stored
//                 as (value - min) in param_stored_width() bytes
//
// A payload shorter than the module's current schema comes from an older
// schema: parameters past its end take their descriptor default. A longer
// one comes from a newer schema: the parameters this firmware doesn't know
// are ignored. The schema version is recorded for tools and diagnostics;
// decoding goes by payload length alone, which append-only IDs make enough.

static constexpr uint8_t kPatchMagic[4] = { 'T', '0', 'P', 'T' };
static constexpr uint32_t kPatchNameLen = 16;
static constexpr uint32_t kPatchHeaderSize = 24;

// A stored patch somewhere in memory (factory bank in flash, a SysEx buffer, ...).
struct StoredPatchRef {
    const uint8_t *data;
    uint32_t len;
};

enum class PatchLoadResult : uint8_t {
    OK,
    BAD_MAGIC,
    WRONG_MODULE,
    TRUNCATED,     // shorter than its header, or than its declared payload
    NO_SUCH_PATCH, // no patch at the requested bank position
};

// Total stored size of a patch for the given descriptor table.
template <uint32_t N>
constexpr uint32_t patch_stored_size(const ParamDescriptor (&table)[N]) {
    uint32_t size = kPatchHeaderSize;
    for (uint32_t i = 0; i < N; i++) size += param_stored_width(table[i]);
    return size;
}

// Encode `values` (one per descriptor, each within its range) as a stored
// patch into `out`. `name` is truncated to kPatchNameLen. Returns the
// number of bytes written, or 0 if `cap` is too small.
template <uint32_t N>
inline uint32_t patch_encode(ModuleId module, uint8_t schema_version, const char *name,
                             const ParamDescriptor (&table)[N], const int32_t *values,
                             uint8_t *out, uint32_t cap) {
    const uint32_t size = patch_stored_size(table);
    if (cap < size) return 0;
    for (uint32_t i = 0; i < 4; i++) out[i] = kPatchMagic[i];
    out[4] = (uint8_t)module;
    out[5] = schema_version;
    bool ended = false;
    for (uint32_t i = 0; i < kPatchNameLen; i++) {
        if (!ended && name[i] == '\0') ended = true;
        out[6 + i] = ended ? 0 : (uint8_t)name[i];
    }
    const uint32_t payload_len = size - kPatchHeaderSize;
    out[22] = (uint8_t)(payload_len & 0xFF);
    out[23] = (uint8_t)(payload_len >> 8);

    uint8_t *p = out + kPatchHeaderSize;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t raw = (uint32_t)(values[i] - table[i].min);
        *p++ = (uint8_t)(raw & 0xFF);
        if (param_stored_width(table[i]) == 2) *p++ = (uint8_t)(raw >> 8);
    }
    return size;
}

inline uint32_t patch_payload_len(const uint8_t *data) {
    return (uint32_t)data[22] | ((uint32_t)data[23] << 8);
}

// Whether `data` (at least 4 bytes) starts with the stored Patch magic.
inline bool patch_has_magic(const uint8_t *data) {
    for (uint32_t i = 0; i < 4; i++) {
        if (data[i] != kPatchMagic[i]) return false;
    }
    return true;
}

// Whether `data` is a stored Patch header for `module` whose payload fits `len`.
inline PatchLoadResult patch_check_header(const uint8_t *data, uint32_t len, ModuleId module) {
    if (len < kPatchHeaderSize) return PatchLoadResult::TRUNCATED;
    if (!patch_has_magic(data)) return PatchLoadResult::BAD_MAGIC;
    if (data[4] != (uint8_t)module) return PatchLoadResult::WRONG_MODULE;
    if (len < kPatchHeaderSize + patch_payload_len(data)) return PatchLoadResult::TRUNCATED;
    return PatchLoadResult::OK;
}

// The name in a stored Patch header into `out` (kPatchNameLen + 1 bytes,
// always terminated).
inline void patch_read_name(const uint8_t *data, char *out) {
    for (uint32_t i = 0; i < kPatchNameLen; i++) out[i] = (char)data[6 + i];
    out[kPatchNameLen] = '\0';
}

// Decode `data` into `values` (one per descriptor) and `name` (kPatchNameLen
// + 1 bytes, always terminated). On any result other than OK, `values` and
// `name` are left untouched.
template <uint32_t N>
inline PatchLoadResult patch_decode(const uint8_t *data, uint32_t len, ModuleId module,
                                    const ParamDescriptor (&table)[N],
                                    int32_t (&values)[N], char *name) {
    const PatchLoadResult header = patch_check_header(data, len, module);
    if (header != PatchLoadResult::OK) return header;
    const uint32_t payload_len = patch_payload_len(data);

    const uint8_t *p = data + kPatchHeaderSize;
    uint32_t remaining = payload_len;
    for (uint32_t i = 0; i < N; i++) {
        const ParamDescriptor &d = table[i];
        uint32_t w = param_stored_width(d);
        if (remaining < w) {
            values[i] = d.def;
            remaining = 0;
            continue;
        }
        uint32_t raw = (w == 1) ? p[0] : ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
        p += w;
        remaining -= w;
        int32_t v = d.min + (int32_t)raw;
        values[i] = v > d.max ? d.max : v;
    }

    patch_read_name(data, name);
    return PatchLoadResult::OK;
}
