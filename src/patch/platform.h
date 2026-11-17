#pragma once

#include "descriptor.h"
#include "midi/nrpn.h"
#include "module_id.h"
#include "part_slot.h"
#include "stored_format.h"
#include <cstdint>

// The module-agnostic Core 0 side of the patch platform: per-Part Edit
// buffers, loading stored Patches into them, cooking, and publishing the
// cooked patch to each Part's slot (docs/platform.md).
//
// `Module` supplies:
//   kModuleId, kSchemaVersion, kPartCount
//   kDescriptors[]                     its descriptor table
//   Cooked                             its engine-native cooked patch
//   static void cook(const int32_t *values, Cooked &out)
template <typename Module>
class PatchPlatformT {
public:
    using Cooked = typename Module::Cooked;
    static constexpr uint32_t kParamCount = sizeof(Module::kDescriptors) / sizeof(Module::kDescriptors[0]);
    static constexpr uint32_t kPartCount = Module::kPartCount;

    static_assert(descriptor_table_valid(Module::kDescriptors), "invalid descriptor table");

    // `slots` is an array of kPartCount, shared with Core 1.
    PatchPlatformT(PartSlotT<Cooked> *slots, const StoredPatchRef *factory, uint32_t factory_count)
        : slots_(slots), factory_(factory), factory_count_(factory_count) {}

    // Load factory Patch `index` into `part`'s Edit buffer and publish it
    // cooked. Out-of-range part or index, or a patch that fails to decode,
    // leaves the Part unchanged.
    PatchLoadResult load_factory(uint8_t part, uint32_t index) {
        if (part >= kPartCount || index >= factory_count_) return PatchLoadResult::NO_SUCH_PATCH;
        return load(part, factory_[index]);
    }

    PatchLoadResult load(uint8_t part, const StoredPatchRef &stored) {
        if (part >= kPartCount) return PatchLoadResult::NO_SUCH_PATCH;
        Edit &e = edit_[part];
        PatchLoadResult r = patch_decode(stored.data, stored.len, Module::kModuleId,
                                         Module::kDescriptors, e.values, e.name);
        if (r != PatchLoadResult::OK) return r;
        e.load_id++;
        Module::cook(e.values, e.cooked);
        e.pending = true;
        publish(part);
        return r;
    }

    // Set one parameter in `part`'s Edit buffer (clamped to its range) and
    // publish the re-cooked patch. Unknown parts or IDs are ignored.
    void set_param(uint8_t part, uint32_t id, int32_t value) {
        if (part >= kPartCount || id >= kParamCount) return;
        const ParamDescriptor &d = Module::kDescriptors[id];
        Edit &e = edit_[part];
        e.values[id] = value < d.min ? d.min : (value > d.max ? d.max : value);
        Module::cook(e.values, e.cooked);
        e.pending = true;
        publish(part);
    }

    // A Control Change on `part`'s channel: NRPN (number = parameter ID,
    // 14-bit value scaled onto the range) or a descriptor's default CC
    // (7-bit value scaled onto the range). Any other CC is ignored.
    void midi_cc(uint8_t part, uint8_t cc, uint8_t value) {
        if (part >= kPartCount) return;
        if (NrpnDecoder::is_nrpn_cc(cc)) {
            uint16_t id, value14;
            if (nrpn_[part].feed(cc, value, id, value14) && id < kParamCount) {
                set_param(part, id, param_value_from_14bit(Module::kDescriptors[id], value14));
            }
            return;
        }
        const uint32_t id = descriptor_find_cc(Module::kDescriptors, cc);
        if (id < kParamCount) set_param(part, id, param_value_from_7bit(Module::kDescriptors[id], value));
    }

    // Core 0, once per main-loop pass: publish any cooked patch a Part's
    // slot refused because Core 1 hadn't taken the previous one yet.
    void task() {
        for (uint32_t p = 0; p < kPartCount; p++) {
            if (edit_[p].pending) publish((uint8_t)p);
        }
    }

    // The latest cook of `part`'s Edit buffer, published or still pending.
    // A note started now takes its note-on fields from here, so it gets
    // every edit made before it even while Core 1 is a buffer behind.
    const Cooked &cooked(uint8_t part) const { return edit_[part].cooked; }

    // Identifies the Patch load `part`'s Edit buffer holds: changes on every
    // load, not on edits. Notes carry it so Core 1 knows which Patch they
    // belong to (receiver.h).
    uint16_t load_id(uint8_t part) const { return edit_[part].load_id; }

    // Encode `part`'s Edit buffer as a stored Patch into `out`; returns the
    // byte count, or 0 if `cap` is too small or `part` out of range.
    uint32_t encode_edit_buffer(uint8_t part, uint8_t *out, uint32_t cap) const {
        if (part >= kPartCount) return 0;
        const Edit &e = edit_[part];
        return patch_encode(Module::kModuleId, Module::kSchemaVersion, e.name, Module::kDescriptors,
                            e.values, out, cap);
    }

    // Name of the Patch in `part`'s Edit buffer.
    const char *name(uint8_t part) const { return edit_[part].name; }

private:
    void publish(uint8_t part) {
        Edit &e = edit_[part];
        if (slots_[part].try_publish(e.cooked, e.load_id)) e.pending = false;
    }

    struct Edit {
        int32_t values[kParamCount];
        char name[kPatchNameLen + 1];
        Cooked cooked;
        uint16_t load_id;  // changes on every load, not on edits
        bool pending;
    };

    PartSlotT<Cooked> *slots_;
    const StoredPatchRef *factory_;
    uint32_t factory_count_;
    Edit edit_[kPartCount]{};
    NrpnDecoder nrpn_[kPartCount]{};
};
