#pragma once

#include "module_id.h"
#include "stored_format.h"
#include <cstdint>
#include <cstring>

// User banks: stored Patches (and, later, Multis) in a reserved region at
// the end of flash, outside the UF2 image, so flashing any module build
// leaves them in place.
//
// The region is split into one fixed-size partition per module ID. Module
// 1's partition is the topmost, each further ID the next one down, so a
// module ID added later extends the region downward without moving any
// existing partition. Every build computes the same layout from these
// constants alone, whichever module it contains, and touches only its own
// partition. All values here are permanent: changing one would orphan
// every Patch already stored.
//
//   partition (48 KB)  offset   size
//     Patch slots      0        128 x 256 B = 32 KB (16 per sector)
//     Multi slots      32 KB    32 x 512 B  = 16 KB (8 per sector)
//
// A slot holds one stored Patch (stored_format.h) followed by 0xFF up to
// the slot size; an erased slot (no Patch magic) is empty. Slots never
// straddle a sector, so a Store rewrites exactly one sector.

static constexpr uint32_t kFlashSectorSize = 4096;

static constexpr uint32_t kUserPatchSlotSize = 256;
static constexpr uint32_t kUserPatchSlots = 128;
static constexpr uint32_t kUserMultiSlotSize = 512;
static constexpr uint32_t kUserMultiSlots = 32;

static constexpr uint32_t kUserPatchAreaSize = kUserPatchSlots * kUserPatchSlotSize;
static constexpr uint32_t kUserMultiAreaSize = kUserMultiSlots * kUserMultiSlotSize;
static constexpr uint32_t kUserPartitionSize = kUserPatchAreaSize + kUserMultiAreaSize;

// Module IDs 1..kUserBankModules have a partition.
static constexpr uint32_t kUserBankModules = 8;
static constexpr uint32_t kUserRegionSize = kUserBankModules * kUserPartitionSize;

static_assert(kFlashSectorSize % kUserPatchSlotSize == 0 && kFlashSectorSize % kUserMultiSlotSize == 0,
              "a slot never straddles a sector");
static_assert(kUserPatchAreaSize % kFlashSectorSize == 0 && kUserPartitionSize % kFlashSectorSize == 0,
              "areas and partitions are sector-aligned");

constexpr bool user_bank_has_partition(ModuleId module) {
    return (uint32_t)module >= 1 && (uint32_t)module <= kUserBankModules;
}

// Offset of `module`'s partition from the start of the region.
constexpr uint32_t user_partition_offset(ModuleId module) {
    return kUserRegionSize - (uint32_t)module * kUserPartitionSize;
}

constexpr uint32_t user_patch_slot_offset(ModuleId module, uint32_t slot) {
    return user_partition_offset(module) + slot * kUserPatchSlotSize;
}

constexpr uint32_t user_multi_slot_offset(ModuleId module, uint32_t slot) {
    return user_partition_offset(module) + kUserPatchAreaSize + slot * kUserMultiSlotSize;
}

// The region as the bank sees it: readable memory (memory-mapped flash on
// the device) and a way to rewrite one sector. `write_sector` erases and
// programs the kFlashSectorSize bytes at `offset` (from the region start,
// sector-aligned) with `data`, returning false if it couldn't. A null
// `base` means there is no usable region.
struct UserFlash {
    const uint8_t *base;
    bool (*write_sector)(uint32_t offset, const uint8_t *data);
};

// Result of a Store; the values go out in the SysEx store reply, so they
// are permanent: append only.
enum class StoreResult : uint8_t {
    OK           = 0,
    NO_SUCH_SLOT = 1,  // slot out of range, unknown Part, or no user bank for this module
    BAD_PATCH    = 2,  // not a valid stored Patch / Multi for this module, or larger than a slot
    FLASH_FAILED = 3,  // the sector write failed or the region is unusable
};

// The two kinds of user-bank slot. The values go out in SysEx bank messages
// (sysex.h), so they are permanent.
enum class SlotKind : uint8_t { PATCH = 0, MULTI = 1 };

// One module's user bank: its Patch slots and its Multi slots. Core 0 only.
class UserBank {
public:
    UserBank(ModuleId module, const UserFlash &flash) : module_(module), flash_(flash) {}

    // The Patch slot, or {nullptr, 0} if there is no such slot. An empty
    // slot is returned too; decoding it reports BAD_MAGIC.
    StoredPatchRef patch(uint32_t slot) const {
        if (!usable() || slot >= kUserPatchSlots) return StoredPatchRef{ nullptr, 0 };
        return StoredPatchRef{ flash_.base + user_patch_slot_offset(module_, slot), kUserPatchSlotSize };
    }

    bool patch_empty(uint32_t slot) const {
        const StoredPatchRef ref = patch(slot);
        return !ref.data || !patch_has_magic(ref.data);
    }

    // Store the stored Patch `data` into `slot`, rewriting the slot's sector
    // and keeping the other slots in it. Storing what the slot already
    // holds writes nothing.
    StoreResult store_patch(uint32_t slot, const uint8_t *data, uint32_t len) {
        if (!usable() || slot >= kUserPatchSlots) return StoreResult::NO_SUCH_SLOT;
        return store(user_patch_slot_offset(module_, slot), kUserPatchSlotSize, kPatchMagic, data, len);
    }

    // The same for Multi slots and stored Multis (multi.h).
    StoredPatchRef multi(uint32_t slot) const {
        if (!usable() || slot >= kUserMultiSlots) return StoredPatchRef{ nullptr, 0 };
        return StoredPatchRef{ flash_.base + user_multi_slot_offset(module_, slot), kUserMultiSlotSize };
    }

    bool multi_empty(uint32_t slot) const {
        const StoredPatchRef ref = multi(slot);
        return !ref.data || !stored_has_magic(ref.data, kMultiMagic);
    }

    StoreResult store_multi(uint32_t slot, const uint8_t *data, uint32_t len) {
        if (!usable() || slot >= kUserMultiSlots) return StoreResult::NO_SUCH_SLOT;
        return store(user_multi_slot_offset(module_, slot), kUserMultiSlotSize, kMultiMagic, data, len);
    }

    // --- By slot kind, for backing up and restoring the whole bank. ---

    // How many slots of `kind` there are; 0 without a usable region.
    uint32_t slot_count(SlotKind kind) const {
        if (!usable()) return 0;
        return kind == SlotKind::PATCH ? kUserPatchSlots : kUserMultiSlots;
    }

    // The stored Patch or Multi in `slot`, without the slot's padding:
    // {nullptr, 0} if the slot holds nothing for this module (empty, or a
    // payload that runs past the slot).
    StoredPatchRef stored(SlotKind kind, uint32_t slot) const {
        if (slot >= slot_count(kind)) return StoredPatchRef{ nullptr, 0 };
        const StoredPatchRef ref = kind == SlotKind::PATCH ? patch(slot) : multi(slot);
        if (stored_check_header(ref.data, ref.len, module_, magic(kind)) != PatchLoadResult::OK) {
            return StoredPatchRef{ nullptr, 0 };
        }
        return StoredPatchRef{ ref.data, kPatchHeaderSize + patch_payload_len(ref.data) };
    }

    // Put `data` into `slot` byte for byte, as a Store does (header checked,
    // padded with 0xFF), or empty the slot if `len` is 0. Writing what the
    // slot already holds writes nothing.
    StoreResult restore(SlotKind kind, uint32_t slot, const uint8_t *data, uint32_t len) {
        if (slot >= slot_count(kind)) return StoreResult::NO_SUCH_SLOT;
        const bool patch = kind == SlotKind::PATCH;
        const uint32_t offset = patch ? user_patch_slot_offset(module_, slot) : user_multi_slot_offset(module_, slot);
        const uint32_t size = patch ? kUserPatchSlotSize : kUserMultiSlotSize;
        if (len == 0) return write_slot(offset, size, data, 0);
        return store(offset, size, magic(kind), data, len);
    }

private:
    bool usable() const { return flash_.base && user_bank_has_partition(module_); }

    static const uint8_t (&magic(SlotKind kind))[4] { return kind == SlotKind::PATCH ? kPatchMagic : kMultiMagic; }

    // Write `data` (a stored header with `magic`, for this module) into the
    // slot of `size` bytes at `offset`, 0xFF-padded, through its sector.
    StoreResult store(uint32_t offset, uint32_t size, const uint8_t (&magic)[4], const uint8_t *data, uint32_t len) {
        if (len > size || stored_check_header(data, len, module_, magic) != PatchLoadResult::OK) {
            return StoreResult::BAD_PATCH;
        }
        return write_slot(offset, size, data, len);
    }

    // Rewrite the slot of `size` bytes at `offset` with `len` bytes of
    // `data`, then 0xFF to its end, through its sector.
    StoreResult write_slot(uint32_t offset, uint32_t size, const uint8_t *data, uint32_t len) {
        const uint32_t sector = offset - offset % kFlashSectorSize;
        for (uint32_t i = 0; i < kFlashSectorSize; i++) sector_[i] = flash_.base[sector + i];
        uint8_t *dst = sector_ + (offset - sector);
        for (uint32_t i = 0; i < size; i++) dst[i] = i < len ? data[i] : 0xFF;
        if (memcmp(sector_, flash_.base + sector, kFlashSectorSize) == 0) return StoreResult::OK;
        return flash_.write_sector(sector, sector_) ? StoreResult::OK : StoreResult::FLASH_FAILED;
    }

    ModuleId module_;
    UserFlash flash_;
    uint8_t sector_[kFlashSectorSize];  // the sector being rewritten; static storage, not the small Core 0 stack
};
