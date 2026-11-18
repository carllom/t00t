// Host test for the user banks (src/patch/user_bank.h) at the UserBank seam:
// Patches stored into and read back from a RAM-backed stand-in for the
// reserved flash region (ram_flash.h).
//
// Stored-patch byte vectors are written out literally, so the test pins the
// slot contents rather than round-tripping through the encoder.

#include "patch/user_bank.h"
#include "ram_flash.h"

#include <cstdio>
#include <cstring>

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

// A subtractive-shaped Patch "LEAD" (module 1, schema 1, 3-byte payload).
const uint8_t kLead[] = {
    'T', '0', 'P', 'T', 0x01, 0x01,
    'L', 'E', 'A', 'D', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x03, 0x00,
    7, 0x34, 0x12,
};

// "PAD" for module 1.
const uint8_t kPad[] = {
    'T', '0', 'P', 'T', 0x01, 0x01,
    'P', 'A', 'D', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x03, 0x00,
    3, 0x00, 0x01,
};

bool slot_holds(const StoredPatchRef &ref, const uint8_t *patch, uint32_t len) {
    return ref.data && ref.len >= len && memcmp(ref.data, patch, len) == 0;
}

// --- Tests ---

void test_a_fresh_region_has_only_empty_slots() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    bool all_empty = true;
    for (uint32_t s = 0; s < kUserPatchSlots; s++) all_empty = all_empty && bank.patch_empty(s);
    check(all_empty, "every Patch slot of an erased region is empty");
}

void test_a_stored_patch_reads_back_from_its_slot() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    check(bank.store_patch(5, kLead, sizeof kLead) == StoreResult::OK, "storing LEAD into slot 5 succeeds");
    check(slot_holds(bank.patch(5), kLead, sizeof kLead), "slot 5 reads back LEAD");
    check(!bank.patch_empty(5) && bank.patch_empty(4) && bank.patch_empty(6), "only slot 5 is filled");
    check(f.writes.size() == 1, "with one sector write");
}

void test_storing_keeps_the_other_slots_of_the_sector() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    bank.store_patch(4, kLead, sizeof kLead);
    bank.store_patch(5, kPad, sizeof kPad);
    check(f.writes.size() == 2 && f.writes[0] == f.writes[1], "slots 4 and 5 share one sector");
    check(slot_holds(bank.patch(4), kLead, sizeof kLead), "storing slot 5 keeps slot 4");
    bank.store_patch(5, kLead, sizeof kLead);
    check(slot_holds(bank.patch(5), kLead, sizeof kLead), "overwriting slot 5 replaces it");
    bool tail_erased = true;
    for (uint32_t i = sizeof kLead; i < kUserPatchSlotSize; i++) tail_erased = tail_erased && bank.patch(5).data[i] == 0xFF;
    check(tail_erased, "the slot past the Patch reads erased");
}

void test_the_layout_is_fixed() {
    // Pinned literally: every module build must agree on these.
    check(kUserRegionSize == 384 * 1024, "the region is 384 KB: 8 partitions of 48 KB");
    check(user_partition_offset(ModuleId::SUBTRACTIVE) == 336 * 1024, "module 1's partition is the topmost");
    check(user_partition_offset(ModuleId::FM) == 240 * 1024, "module 3's is two partitions below");
    check(user_patch_slot_offset(ModuleId::SUBTRACTIVE, 17) == 336 * 1024 + 17 * 256, "Patch slots are 256 B from the partition start");
    check(user_multi_slot_offset(ModuleId::SUBTRACTIVE, 3) == 336 * 1024 + 32 * 1024 + 3 * 512, "Multi slots are 512 B after the 32 KB of Patch slots");
    check(user_multi_slot_offset(ModuleId::SUBTRACTIVE, kUserMultiSlots - 1) + kUserMultiSlotSize == kUserRegionSize,
          "module 1's last Multi slot ends the region");
    check(user_partition_offset((ModuleId)kUserBankModules) == 0, "the last partition starts the region");
    check(!user_bank_has_partition(ModuleId::NONE) && !user_bank_has_partition((ModuleId)(kUserBankModules + 1)),
          "module ID 0 and IDs past the reserved ones have no partition");
}

void test_each_module_stores_into_its_own_partition() {
    RamFlash f;
    UserBank sub(ModuleId::SUBTRACTIVE, user_flash(f));
    UserBank fm(ModuleId::FM, user_flash(f));
    uint8_t fm_patch[sizeof kPad];
    memcpy(fm_patch, kPad, sizeof kPad);
    fm_patch[4] = (uint8_t)ModuleId::FM;

    sub.store_patch(0, kLead, sizeof kLead);
    fm.store_patch(0, fm_patch, sizeof fm_patch);
    check(slot_holds(sub.patch(0), kLead, sizeof kLead), "subtractive slot 0 holds LEAD");
    check(slot_holds(fm.patch(0), fm_patch, sizeof fm_patch), "FM slot 0 holds its own Patch");
    check(f.writes.size() == 2 && f.writes[0] != f.writes[1], "in different sectors");
}

void test_invalid_stores_are_rejected_and_write_nothing() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    uint8_t foreign[sizeof kLead];
    memcpy(foreign, kLead, sizeof kLead);
    foreign[4] = (uint8_t)ModuleId::FM;
    uint8_t big[kUserPatchSlotSize + 1] = {};
    memcpy(big, kLead, sizeof kLead);

    check(bank.store_patch(kUserPatchSlots, kLead, sizeof kLead) == StoreResult::NO_SUCH_SLOT, "a slot past the bank is refused");
    check(bank.store_patch(0, foreign, sizeof foreign) == StoreResult::BAD_PATCH, "another module's Patch is refused");
    check(bank.store_patch(0, kLead, sizeof kLead - 1) == StoreResult::BAD_PATCH, "a Patch shorter than its header says is refused");
    check(bank.store_patch(0, big, sizeof big) == StoreResult::BAD_PATCH, "a Patch larger than a slot is refused");
    UserBank none(ModuleId::NONE, user_flash(f));
    check(none.store_patch(0, kLead, sizeof kLead) == StoreResult::NO_SUCH_SLOT && none.patch(0).data == nullptr,
          "a module without a partition has no slots");
    UserBank absent(ModuleId::SUBTRACTIVE, UserFlash{ nullptr, ram_flash_write_sector });
    check(absent.store_patch(0, kLead, sizeof kLead) == StoreResult::NO_SUCH_SLOT && absent.patch_empty(0),
          "without a usable region every slot is empty and stores are refused");
    check(f.writes.empty(), "none of that wrote flash");
}

void test_storing_an_unchanged_patch_writes_nothing() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    bank.store_patch(9, kLead, sizeof kLead);
    check(bank.store_patch(9, kLead, sizeof kLead) == StoreResult::OK && f.writes.size() == 1,
          "storing what the slot already holds succeeds without a flash write");
}

void test_a_failed_write_is_reported_and_the_slot_unchanged() {
    RamFlash f;
    UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
    bank.store_patch(2, kLead, sizeof kLead);
    f.fail = true;
    check(bank.store_patch(2, kPad, sizeof kPad) == StoreResult::FLASH_FAILED, "a failed sector write reports FLASH_FAILED");
    check(slot_holds(bank.patch(2), kLead, sizeof kLead), "and the slot keeps its Patch");
}

void test_patches_survive_a_restart_and_another_module_build() {
    RamFlash f;
    {
        UserBank bank(ModuleId::SUBTRACTIVE, user_flash(f));
        bank.store_patch(127, kPad, sizeof kPad);
    }
    {
        // Another module's build runs on the same flash and stores its own.
        UserBank fm(ModuleId::FM, user_flash(f));
        uint8_t fm_patch[sizeof kLead];
        memcpy(fm_patch, kLead, sizeof kLead);
        fm_patch[4] = (uint8_t)ModuleId::FM;
        fm.store_patch(127, fm_patch, sizeof fm_patch);
    }
    UserBank again(ModuleId::SUBTRACTIVE, user_flash(f));
    check(slot_holds(again.patch(127), kPad, sizeof kPad), "the subtractive Patch is still there afterwards");
}

}  // namespace

int main() {
    test_a_fresh_region_has_only_empty_slots();
    test_a_stored_patch_reads_back_from_its_slot();
    test_storing_keeps_the_other_slots_of_the_sector();
    test_the_layout_is_fixed();
    test_each_module_stores_into_its_own_partition();
    test_invalid_stores_are_rejected_and_write_nothing();
    test_storing_an_unchanged_patch_writes_nothing();
    test_a_failed_write_is_reported_and_the_slot_unchanged();
    test_patches_survive_a_restart_and_another_module_build();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
