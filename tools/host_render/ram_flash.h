#pragma once

// A RAM-backed stand-in for the reserved user-bank flash region, for host
// tests. Behaves like NOR flash -- a sector write erases to 0xFF and then
// programs -- and records every sector it writes. One instance is current
// at a time (the write callback is a plain function pointer).

#include "patch/user_bank.h"

#include <cstring>
#include <vector>

struct RamFlash {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kUserRegionSize, 0xFF);
    std::vector<uint32_t> writes;  // sector offsets, in write order
    bool fail = false;             // writes fail, leaving flash untouched

    // Back to a freshly erased region, in place (banks keep pointing at it).
    void reset() {
        for (auto &b : mem) b = 0xFF;
        writes.clear();
        fail = false;
    }

    // Write `len` bytes straight into the region, bypassing the bank -- for
    // planting contents a store would refuse.
    void poke(uint32_t offset, const uint8_t *data, uint32_t len) { memcpy(mem.data() + offset, data, len); }
};

inline RamFlash *&ram_flash_current() {
    static RamFlash *current = nullptr;
    return current;
}

inline bool ram_flash_write_sector(uint32_t offset, const uint8_t *data) {
    RamFlash &f = *ram_flash_current();
    if (f.fail) return false;
    f.writes.push_back(offset);
    for (uint32_t i = 0; i < kFlashSectorSize; i++) f.mem[offset + i] = 0xFF;      // erase
    for (uint32_t i = 0; i < kFlashSectorSize; i++) f.mem[offset + i] &= data[i];  // program
    return true;
}

// The UserFlash for `f`, which becomes the current instance.
inline UserFlash user_flash(RamFlash &f) {
    ram_flash_current() = &f;
    return UserFlash{ f.mem.data(), ram_flash_write_sector };
}
