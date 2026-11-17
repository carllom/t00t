#pragma once

#include "descriptor.h"
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
        Edit &e = edit_[part];
        PatchLoadResult r = patch_decode(stored.data, stored.len, Module::kModuleId,
                                         Module::kDescriptors, e.values, e.name);
        if (r != PatchLoadResult::OK) return r;
        Module::cook(e.values, e.cooked);
        e.pending = true;
        publish(part);
        return r;
    }

    // Core 0, once per main-loop pass: publish any cooked patch a Part's
    // slot refused because Core 1 hadn't taken the previous one yet.
    void task() {
        for (uint32_t p = 0; p < kPartCount; p++) {
            if (edit_[p].pending) publish((uint8_t)p);
        }
    }

    // The cooked patch Core 1 renders `part` with from its next buffer on --
    // the last one published, not a newer one still waiting in the Edit
    // buffer -- so Core 0's note-on maths agrees with what Core 1 plays.
    const Cooked &cooked(uint8_t part) const { return published_[part]; }

    // The slot generation of cooked(part), for tagging state derived from it.
    uint32_t cooked_generation(uint8_t part) const { return published_gen_[part]; }

    // Name of the Patch in `part`'s Edit buffer.
    const char *name(uint8_t part) const { return edit_[part].name; }

private:
    void publish(uint8_t part) {
        Edit &e = edit_[part];
        if (slots_[part].try_publish(e.cooked, &published_gen_[part])) {
            e.pending = false;
            published_[part] = e.cooked;
        }
    }

    struct Edit {
        int32_t values[kParamCount];
        char name[kPatchNameLen + 1];
        Cooked cooked;
        bool pending;
    };

    PartSlotT<Cooked> *slots_;
    const StoredPatchRef *factory_;
    uint32_t factory_count_;
    Edit edit_[kPartCount]{};
    Cooked published_[kPartCount]{};
    uint32_t published_gen_[kPartCount]{};
};
