#pragma once

#include "common.h"
#include "multi_cooked.h"
#include "part_slot.h"
#include "platform.h"
#include "stored_format.h"
#include "user_bank.h"
#include <cstdint>

// The Multi (CONTEXT.md "Multi", docs/platform.md): a module's Parts --
// each with a MIDI channel, a Patch (by bank and program), volume, pan and
// FX send -- plus the settings they share, the FX and the master level.
// Core 0 owns it; Core 1 sees only its cooked form through the Multi slot.
//
// Channel routing: a MIDI channel event reaches every Part on that channel
// (Parts on one channel layer), none if no Part is on it. Program Change
// loads a Patch into those Parts and updates their references; it never
// changes the Multi's own settings, and a Multi is never selected by one.
//
// Stored Multi (in a user-bank Multi slot, or over SysEx), little-endian:
//
//   0    24   header as for a Patch (stored_format.h), magic "T0MT"
//   24   1    common bytes C
//   25   C    kCommonDescriptors values, in ID order, as (value - min)
//   ...  1    Part count P
//   ...  1    Part bytes B
//   ...  P*B  each Part's kPartDescriptors values, likewise
//
// Values a stored Multi lacks (an older schema) take their descriptor
// defaults; Parts it lacks take the default Multi's; extra values and
// Parts are ignored.

static constexpr uint8_t kMultiSchemaVersion = 1;

template <uint32_t N>
constexpr uint32_t descriptor_bytes(const ParamDescriptor (&table)[N]) {
    uint32_t n = 0;
    for (const ParamDescriptor &d : table) n += param_stored_width(d);
    return n;
}

// `Module` supplies, besides what PatchPlatformT needs:
//   static constexpr uint8_t default_program(uint32_t part)
//                           the default Multi's Patch for each Part, from
//                           factory bank 0
template <typename Module>
class MultiT {
public:
    using Platform = PatchPlatformT<Module>;
    static constexpr uint32_t kPartCount = Module::kPartCount;
    static_assert(kPartCount <= 32, "parts_on_channel() is a 32-bit mask");
    using Cooked = MultiCookedT<kPartCount>;

    static constexpr uint32_t kStoredSize = kPatchHeaderSize + 1 + descriptor_bytes(kCommonDescriptors) + 2 +
                                            kPartCount * descriptor_bytes(kPartDescriptors);
    static_assert(kStoredSize <= kUserMultiSlotSize, "this module's stored Multi doesn't fit a Multi slot");

    // `user` may be null (no Multi slots); `slot` is shared with Core 1.
    MultiT(Platform &platform, UserBank *user, PartSlotT<Cooked> &slot)
        : platform_(platform), user_(user), slot_(slot) {}

    // The default Multi: Part n on channel n with its default Patch, the
    // descriptor defaults for everything else. Loads the Patches.
    void init() {
        Values v;
        default_values(v);
        set_name(v, "INIT");
        apply(v);
        origin_ = -1;
    }

    // Bitmask of the Parts listening on MIDI channel `channel`.
    uint32_t parts_on_channel(uint8_t channel) const {
        uint32_t mask = 0;
        for (uint32_t p = 0; p < kPartCount; p++) {
            if (values_.part[p][PART_P_CHANNEL] == channel) mask |= 1u << p;
        }
        return mask;
    }

    // The lowest Part on `channel`, or kPartCount if none.
    uint8_t first_part_on_channel(uint8_t channel) const {
        const uint32_t mask = parts_on_channel(channel);
        return mask ? (uint8_t)__builtin_ctz(mask) : (uint8_t)kPartCount;
    }

    // Program Change on `channel` with Bank Select MSB `bank_msb`: load the
    // Patch into every Part on the channel, which then refers to it. A
    // missing Patch changes nothing.
    void program_change(uint8_t channel, uint8_t bank_msb, uint8_t program) {
        for_parts(channel, [&](uint8_t p) {
            if (platform_.load_program(p, bank_msb, program) != PatchLoadResult::OK) return;
            values_.part[p][PART_P_BANK] = bank_msb;
            values_.part[p][PART_P_PROGRAM] = program;
            revision_++;
        });
    }

    // NRPN numbers past the Patch parameters: MSB 126 addresses a Part
    // parameter (LSB = its ID) on the channel's Parts, MSB 127 one of the
    // Multi's own settings from any channel.
    static constexpr uint16_t kNrpnPart = 126 << 7;
    static constexpr uint16_t kNrpnMulti = 127 << 7;
    static_assert(Platform::kParamCount <= kNrpnPart, "Patch parameter IDs reach the Multi's NRPN numbers");

    // Control Change on `channel`: an NRPN for a Part or Multi parameter
    // (kNrpnPart / kNrpnMulti), or a Part parameter's default CC (volume
    // CC7, pan CC10, FX send CC91), sets it -- for every Part on the
    // channel; any other CC goes to those Parts' Patches
    // (PatchPlatformT::midi_cc(), which also sees every NRPN CC).
    void control_change(uint8_t channel, uint8_t cc, uint8_t value) {
        if (NrpnDecoder::is_nrpn_cc(cc) && channel < 16) {
            uint16_t nrpn, value14;
            if (nrpn_[channel].feed(cc, value, nrpn, value14) && nrpn >= kNrpnPart) {
                const uint32_t pid = nrpn - (nrpn >= kNrpnMulti ? kNrpnMulti : kNrpnPart);
                if (nrpn >= kNrpnMulti) {
                    if (pid < COMMON_P_COUNT) set_common(pid, param_value_from_14bit(kCommonDescriptors[pid], value14));
                } else if (pid < PART_P_COUNT) {
                    for_parts(channel, [&](uint8_t p) {
                        set_part(p, pid, param_value_from_14bit(kPartDescriptors[pid], value14));
                    });
                }
                return;
            }
        }
        const uint32_t id = descriptor_find_cc(kPartDescriptors, cc);
        for_parts(channel, [&](uint8_t p) {
            if (id < PART_P_COUNT) set_part(p, id, param_value_from_7bit(kPartDescriptors[id], value));
            else platform_.midi_cc(p, cc, value);
        });
    }

    // Set one of the Multi's own settings (clamped).
    void set_common(uint32_t id, int32_t value) {
        if (id >= COMMON_P_COUNT) return;
        values_.common[id] = clamp(kCommonDescriptors[id], value);
        changed();
    }

    // Set one Part parameter (clamped). A new bank or program loads that
    // Patch into the Part, and is refused if there is none.
    void set_part(uint8_t part, uint32_t id, int32_t value) {
        if (part >= kPartCount || id >= PART_P_COUNT) return;
        value = clamp(kPartDescriptors[id], value);
        if (id == PART_P_BANK || id == PART_P_PROGRAM) {
            const int32_t bank = id == PART_P_BANK ? value : values_.part[part][PART_P_BANK];
            const int32_t program = id == PART_P_PROGRAM ? value : values_.part[part][PART_P_PROGRAM];
            if (platform_.load_program(part, (uint8_t)bank, (uint8_t)program) != PatchLoadResult::OK) return;
        }
        values_.part[part][id] = value;
        changed();
    }

    int32_t common(uint32_t id) const { return values_.common[id]; }
    int32_t part_param(uint8_t part, uint32_t id) const { return values_.part[part][id]; }

    const char *name() const { return values_.name; }
    void rename(const char *name) {
        set_name(values_, name);
        revision_++;
    }

    // The Multi slot this Multi was loaded from or stored into, or -1.
    int32_t origin_slot() const { return origin_; }

    // Changes whenever anything a display shows changes.
    uint32_t revision() const { return revision_; }

    // Load the Multi in user Multi slot `slot`; an empty slot is NO_SUCH_PATCH.
    PatchLoadResult load(uint32_t slot) {
        if (!user_ || user_->multi_empty(slot)) return PatchLoadResult::NO_SUCH_PATCH;
        const PatchLoadResult r = load_stored(user_->multi(slot));
        if (r == PatchLoadResult::OK) origin_ = (int32_t)slot;
        return r;
    }

    // Load a stored Multi: its settings, then each Part's Patch by
    // reference; a Part whose Patch can't be loaded gets factory bank 0,
    // slot 0. On anything but OK the Multi is unchanged.
    PatchLoadResult load_stored(const StoredPatchRef &stored) {
        Values v;
        const PatchLoadResult r = decode(stored.data, stored.len, v);
        if (r != PatchLoadResult::OK) return r;
        apply(v);
        origin_ = -1;
        return r;
    }

    // Store this Multi into user Multi slot `slot`.
    StoreResult store(uint32_t slot) {
        if (!user_) return StoreResult::NO_SUCH_SLOT;
        uint8_t buf[kStoredSize];
        const StoreResult r = user_->store_multi(slot, buf, encode(buf, sizeof buf));
        if (r == StoreResult::OK) {
            origin_ = (int32_t)slot;
            revision_++;
        }
        return r;
    }

    // Encode this Multi as a stored Multi into `out`; the byte count, or 0
    // if `cap` is too small.
    uint32_t encode(uint8_t *out, uint32_t cap) const {
        if (cap < kStoredSize) return 0;
        stored_write_header(out, kMultiMagic, Module::kModuleId, kMultiSchemaVersion, values_.name,
                            kStoredSize - kPatchHeaderSize);
        uint8_t *p = out + kPatchHeaderSize;
        *p++ = (uint8_t)descriptor_bytes(kCommonDescriptors);
        for (uint32_t i = 0; i < COMMON_P_COUNT; i++) p += param_store(kCommonDescriptors[i], values_.common[i], p);
        *p++ = (uint8_t)kPartCount;
        *p++ = (uint8_t)descriptor_bytes(kPartDescriptors);
        for (uint32_t part = 0; part < kPartCount; part++) {
            for (uint32_t i = 0; i < PART_P_COUNT; i++) p += param_store(kPartDescriptors[i], values_.part[part][i], p);
        }
        return kStoredSize;
    }

    // The name of the Multi in user Multi slot `slot` into `out`
    // (kPatchNameLen + 1 bytes); false if the slot is empty.
    bool user_slot_name(uint32_t slot, char *out) const {
        if (!user_ || user_->multi_empty(slot)) return false;
        patch_read_name(user_->multi(slot).data, out);
        return true;
    }

    // Core 0, once per main-loop pass: publish a cooked Multi the slot
    // refused because Core 1 hadn't taken the previous one yet.
    void task() {
        if (pending_) publish();
    }

private:
    struct Values {
        int32_t common[COMMON_P_COUNT];
        int32_t part[kPartCount][PART_P_COUNT];
        char name[kPatchNameLen + 1];
    };

    template <typename F>
    void for_parts(uint8_t channel, F f) {
        const uint32_t mask = parts_on_channel(channel);
        for (uint32_t p = 0; p < kPartCount; p++) {
            if (mask & (1u << p)) f((uint8_t)p);
        }
    }

    static int32_t clamp(const ParamDescriptor &d, int32_t v) { return v < d.min ? d.min : (v > d.max ? d.max : v); }

    static void set_name(Values &v, const char *name) {
        bool ended = false;
        for (uint32_t i = 0; i < kPatchNameLen; i++) {
            if (!ended && name[i] == '\0') ended = true;
            v.name[i] = ended ? '\0' : name[i];
        }
        v.name[kPatchNameLen] = '\0';
    }

    static void default_part(Values &v, uint32_t p) {
        for (uint32_t i = 0; i < PART_P_COUNT; i++) v.part[p][i] = kPartDescriptors[i].def;
        v.part[p][PART_P_CHANNEL] = p < 16 ? (int32_t)p : kPartChannelOff;
        v.part[p][PART_P_PROGRAM] = Module::default_program(p);
    }

    static void default_values(Values &v) {
        for (uint32_t i = 0; i < COMMON_P_COUNT; i++) v.common[i] = kCommonDescriptors[i].def;
        for (uint32_t p = 0; p < kPartCount; p++) default_part(v, p);
    }

    // Read `count` values of `table` from `bytes` stored bytes at `in`;
    // values past them keep their defaults.
    template <uint32_t N>
    static void read_values(const ParamDescriptor (&table)[N], const uint8_t *in, uint32_t bytes, int32_t *out) {
        for (uint32_t i = 0; i < N; i++) {
            const uint32_t w = param_stored_width(table[i]);
            if (bytes < w) return;
            out[i] = param_load(table[i], in);
            in += w;
            bytes -= w;
        }
    }

    static PatchLoadResult decode(const uint8_t *data, uint32_t len, Values &v) {
        PatchLoadResult r = stored_check_header(data, len, Module::kModuleId, kMultiMagic);
        if (r != PatchLoadResult::OK) return r;
        const uint8_t *p = data + kPatchHeaderSize;
        const uint8_t *end = p + patch_payload_len(data);

        default_values(v);
        patch_read_name(data, v.name);
        if (end - p < 1) return PatchLoadResult::TRUNCATED;
        const uint32_t common_bytes = *p++;
        if ((uint32_t)(end - p) < common_bytes + 2) return PatchLoadResult::TRUNCATED;
        read_values(kCommonDescriptors, p, common_bytes, v.common);
        p += common_bytes;
        const uint32_t parts = *p++;
        const uint32_t part_bytes = *p++;
        if ((uint32_t)(end - p) < parts * part_bytes) return PatchLoadResult::TRUNCATED;
        for (uint32_t i = 0; i < parts && i < kPartCount; i++) {
            read_values(kPartDescriptors, p + i * part_bytes, part_bytes, v.part[i]);
        }
        return PatchLoadResult::OK;
    }

    // Make `v` this Multi: load each Part's Patch, cook and publish.
    void apply(Values &v) {
        for (uint8_t p = 0; p < kPartCount; p++) {
            const uint8_t bank = (uint8_t)v.part[p][PART_P_BANK], program = (uint8_t)v.part[p][PART_P_PROGRAM];
            if (platform_.load_program(p, bank, program) != PatchLoadResult::OK) {
                platform_.load_factory(p, 0, 0);
                v.part[p][PART_P_BANK] = 0;
                v.part[p][PART_P_PROGRAM] = 0;
            }
        }
        values_ = v;
        changed();
    }

    void changed() {
        revision_++;
        cook();
        pending_ = true;
        publish();
    }

    void cook() { multi_cook(values_.common, values_.part, cooked_); }

    void publish() {
        if (slot_.try_publish(cooked_, 0)) pending_ = false;
    }

    Platform &platform_;
    UserBank *user_;
    PartSlotT<Cooked> &slot_;
    Values values_{};
    Cooked cooked_{};
    int32_t origin_ = -1;
    uint32_t revision_ = 0;
    bool pending_ = false;
    NrpnDecoder nrpn_[16]{};
};
