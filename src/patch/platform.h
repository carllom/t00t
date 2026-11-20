#pragma once

#include "descriptor.h"
#include "midi/nrpn.h"
#include "module_id.h"
#include "part_slot.h"
#include "profile.h"
#include "stored_format.h"
#include "user_bank.h"
#include <cstdint>

// A compiled-in, read-only factory bank of stored Patches.
struct FactoryBank {
    const StoredPatchRef *patches;
    uint32_t count;
};

// Bank Select MSB 0..63 picks factory bank MSB, 64..127 user bank MSB - 64;
// Program Change then picks the slot in that bank.
static constexpr uint8_t kUserBankMsb = 64;
static constexpr uint32_t kBankSize = 128;

// Where an Edit buffer's stored version lives.
enum class PatchSource : uint8_t {
    NONE,     // not from a bank (a SysEx edit-buffer dump): its stored version is what arrived
    FACTORY,
    USER,
};

struct PatchOrigin {
    PatchSource source;
    uint8_t bank;   // FACTORY: factory bank
    uint8_t slot;   // FACTORY: index in the bank; USER: user slot
};

// The module-agnostic Core 0 side of the patch platform: per-Part Edit
// buffers, loading stored Patches into them, cooking, and publishing the
// cooked patch to each Part's slot (docs/platform.md), and storing Edit
// buffers into the module's user bank.
//
// Each Edit buffer keeps a copy of its stored version -- the Patch as it
// was loaded or last stored -- so it can tell whether it is edited and
// Compare the two. Compare publishes the stored version in place of the
// Edit buffer; any edit ends it.
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
    static_assert(patch_stored_size(Module::kDescriptors) <= kUserPatchSlotSize,
                  "this module's stored Patch doesn't fit a user-bank slot");

    // `slots` is an array of kPartCount, shared with Core 1. `factory` holds
    // `factory_count` banks (at most kUserBankMsb); `user` may be null for
    // no user bank.
    PatchPlatformT(PartSlotT<Cooked> *slots, const FactoryBank *factory, uint32_t factory_count,
                   UserBank *user)
        : slots_(slots), factory_(factory), factory_count_(factory_count), user_(user) {}

    // Load the Patch at `index` of factory bank `bank` into `part`'s Edit
    // buffer and publish it cooked. Out-of-range part, bank or index, or a
    // patch that fails to decode, leaves the Part unchanged.
    PatchLoadResult load_factory(uint8_t part, uint32_t bank, uint32_t index) {
        if (bank >= factory_count_ || index >= factory_[bank].count) return PatchLoadResult::NO_SUCH_PATCH;
        const PatchLoadResult r = load(part, factory_[bank].patches[index]);
        if (r == PatchLoadResult::OK) edit_[part].origin = PatchOrigin{ PatchSource::FACTORY, (uint8_t)bank, (uint8_t)index };
        return r;
    }

    // Load user slot `slot`; an empty slot is NO_SUCH_PATCH.
    PatchLoadResult load_user(uint8_t part, uint32_t slot) {
        if (!user_ || user_->patch_empty(slot)) return PatchLoadResult::NO_SUCH_PATCH;
        const PatchLoadResult r = load(part, user_->patch(slot));
        if (r == PatchLoadResult::OK) edit_[part].origin = PatchOrigin{ PatchSource::USER, 0, (uint8_t)slot };
        return r;
    }

    // Program Change `program` with Bank Select MSB `bank_msb`: load the
    // slot from the factory or user bank the MSB picks.
    PatchLoadResult load_program(uint8_t part, uint8_t bank_msb, uint8_t program) {
        if (bank_msb < kUserBankMsb) return load_factory(part, bank_msb, program);
        return load_user(part, (uint32_t)(bank_msb - kUserBankMsb) * kBankSize + program);
    }

    // Store `part`'s Edit buffer into user slot `slot`, which becomes its
    // stored version. Blocks for the sector write (audio is silent
    // meanwhile on the device).
    StoreResult store(uint8_t part, uint32_t slot) {
        if (!user_ || part >= kPartCount) return StoreResult::NO_SUCH_SLOT;
        uint8_t stored[patch_stored_size(Module::kDescriptors)];
        const uint32_t n = encode_edit_buffer(part, stored, sizeof stored);
        const StoreResult r = user_->store_patch(slot, stored, n);
        if (r == StoreResult::OK) {
            set_compare(part, false);
            Edit &e = edit_[part];
            keep_as_stored(e);
            e.origin = PatchOrigin{ PatchSource::USER, 0, (uint8_t)slot };
            e.revision++;
        }
        return r;
    }

    // Load a stored Patch into `part`, replacing its Edit buffer and its
    // stored version, with Compare off. Its origin is NONE; the factory and
    // user loads above set theirs.
    PatchLoadResult load(uint8_t part, const StoredPatchRef &stored) {
        if (part >= kPartCount) return PatchLoadResult::NO_SUCH_PATCH;
        PATCH_PROFILE_START(load_start);
        Edit &e = edit_[part];
        PatchLoadResult r = patch_decode(stored.data, stored.len, Module::kModuleId,
                                         Module::kDescriptors, e.values, e.name);
        if (r != PatchLoadResult::OK) return r;
        keep_as_stored(e);
        e.origin = PatchOrigin{ PatchSource::NONE, 0, 0 };
        e.comparing = false;
        e.revision++;
        e.load_id++;
        cook(e);
        e.pending = true;
        publish(part);
        PATCH_PROFILE_RECORD(load, load_start);
        return r;
    }

    // Set one parameter in `part`'s Edit buffer (clamped to its range) and
    // publish the re-cooked patch, ending Compare. Unknown parts or IDs are
    // ignored.
    void set_param(uint8_t part, uint32_t id, int32_t value) {
        if (part >= kPartCount || id >= kParamCount) return;
        PATCH_PROFILE_START(edit_start);
        const ParamDescriptor &d = Module::kDescriptors[id];
        Edit &e = edit_[part];
        e.values[id] = value < d.min ? d.min : (value > d.max ? d.max : value);
        e.comparing = false;
        e.revision++;
        cook(e);
        e.pending = true;
        publish(part);
        PATCH_PROFILE_RECORD(edit, edit_start);
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

    // The latest cook of what `part` plays -- its Edit buffer, or its
    // stored version while comparing -- published or still pending.
    // A note started now takes its note-on fields from here, so it gets
    // every edit made before it even while Core 1 is a buffer behind.
    const Cooked &cooked(uint8_t part) const { return edit_[part].cooked; }

    // Identifies the Patch load `part`'s Edit buffer holds: changes on every
    // load, not on edits. Notes carry it so Core 1 knows which Patch they
    // belong to (receiver.h).
    uint16_t load_id(uint8_t part) const { return edit_[part].load_id; }

    // Changes on every cook of `part`, published with it (part_slot.h): a
    // note carrying it can tell whether Core 1 has its cook yet.
    uint16_t cook_seq(uint8_t part) const { return edit_[part].cook_seq; }

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

    // Rename `part`'s Edit buffer (cut to kPatchNameLen), ending Compare.
    void rename(uint8_t part, const char *name) {
        if (part >= kPartCount) return;
        Edit &e = edit_[part];
        bool ended = false;
        for (uint32_t i = 0; i < kPatchNameLen; i++) {
            if (!ended && name[i] == '\0') ended = true;
            e.name[i] = ended ? '\0' : name[i];
        }
        e.name[kPatchNameLen] = '\0';
        if (e.comparing) set_compare(part, false);
        e.revision++;
    }

    // One parameter of `part`'s Edit buffer, and of its stored version.
    int32_t param(uint8_t part, uint32_t id) const { return edit_[part].values[id]; }
    int32_t stored_param(uint8_t part, uint32_t id) const { return edit_[part].stored_values[id]; }

    // Whether `part`'s Edit buffer differs from its stored version.
    bool edited(uint8_t part) const {
        const Edit &e = edit_[part];
        for (uint32_t i = 0; i < kParamCount; i++) {
            if (e.values[i] != e.stored_values[i]) return true;
        }
        for (uint32_t i = 0; i < kPatchNameLen; i++) {
            if (e.name[i] != e.stored_name[i]) return true;
            if (!e.name[i]) break;
        }
        return false;
    }

    // Compare: publish `part`'s stored version (on) or its Edit buffer
    // (off) without touching either. Notes keep following the Part, as for
    // an edit.
    void set_compare(uint8_t part, bool on) {
        if (part >= kPartCount) return;
        Edit &e = edit_[part];
        if (e.comparing == on) return;
        e.comparing = on;
        e.revision++;
        cook(e);
        e.pending = true;
        publish(part);
    }

    bool comparing(uint8_t part) const { return edit_[part].comparing; }

    PatchOrigin origin(uint8_t part) const { return edit_[part].origin; }

    // Changes whenever anything about `part`'s Edit buffer a display shows
    // changes: a load, an edit, a rename, Compare, a Store.
    uint32_t revision(uint8_t part) const { return edit_[part].revision; }

    // The name of the Patch in user slot `slot` into `out` (kPatchNameLen +
    // 1 bytes, terminated); false if the slot is empty or doesn't exist.
    bool user_slot_name(uint32_t slot, char *out) const {
        if (!user_ || user_->patch_empty(slot)) return false;
        patch_read_name(user_->patch(slot).data, out);
        return true;
    }

private:
    struct Edit;

    // What's heard: the stored version while comparing, else the Edit buffer.
    void cook(Edit &e) {
        PATCH_PROFILE_START(cook_start);
        Module::cook(e.comparing ? e.stored_values : e.values, e.cooked);
        e.cook_seq++;
        PATCH_PROFILE_RECORD(cook, cook_start);
    }

    static void keep_as_stored(Edit &e) {
        for (uint32_t i = 0; i < kParamCount; i++) e.stored_values[i] = e.values[i];
        for (uint32_t i = 0; i <= kPatchNameLen; i++) e.stored_name[i] = e.name[i];
    }

    void publish(uint8_t part) {
        Edit &e = edit_[part];
        if (slots_[part].try_publish(e.cooked, e.load_id, e.cook_seq)) e.pending = false;
    }

    struct Edit {
        int32_t values[kParamCount];
        char name[kPatchNameLen + 1];
        int32_t stored_values[kParamCount];  // the stored version
        char stored_name[kPatchNameLen + 1];
        PatchOrigin origin;
        Cooked cooked;     // of what's heard (see cook())
        uint32_t revision;
        uint16_t load_id;  // changes on every load, not on edits
        uint16_t cook_seq; // changes on every cook
        bool comparing;
        bool pending;
    };

    PartSlotT<Cooked> *slots_;
    const FactoryBank *factory_;
    uint32_t factory_count_;
    UserBank *user_;
    Edit edit_[kPartCount]{};
    NrpnDecoder nrpn_[kPartCount]{};
};
