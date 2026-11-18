#include "user_flash.h"
#include "core1_park.h"
#include "output.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/structs/nvic.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include <cstring>

static constexpr uint32_t kRegionOffset = PICO_FLASH_SIZE_BYTES - kUserRegionSize;
static_assert(kRegionOffset % kFlashSectorSize == 0, "the region starts on a sector");
static_assert(kRegionOffset == 0x3A0000, "CMakeLists.txt's image-size check assumes the region starts at 0x3A0000");

// Core 1 parks at its next buffer, at most one buffer period away.
static constexpr uint32_t kParkTimeoutUs = 50000;

extern "C" char __flash_binary_end;

static bool write_sector(uint32_t offset, const uint8_t *data) {
    if (offset % kFlashSectorSize != 0 || offset + kFlashSectorSize > kUserRegionSize) return false;
    if (!core1_park(kParkTimeoutUs)) return false;
    i2s_output_silence();

    // The audio DMA handler runs from RAM and keeps the I2S clocks going
    // (playing the silenced buffers); every other handler may be in flash.
    constexpr uint32_t kWords = sizeof nvic_hw->iser / sizeof nvic_hw->iser[0];
    uint32_t enabled[kWords];
    for (uint32_t w = 0; w < kWords; w++) {
        const uint32_t keep = (I2S_OUTPUT_IRQ / 32 == w) ? 1u << (I2S_OUTPUT_IRQ % 32) : 0;
        enabled[w] = nvic_hw->iser[w];
        nvic_hw->icer[w] = enabled[w] & ~keep;
    }
    __dsb();
    __isb();

    const uint32_t at = kRegionOffset + offset;
    flash_range_erase(at, kFlashSectorSize);
    flash_range_program(at, data, kFlashSectorSize);

    for (uint32_t w = 0; w < kWords; w++) nvic_hw->iser[w] = enabled[w];
    core1_unpark();

    return memcmp((const void *)(XIP_BASE + at), data, kFlashSectorSize) == 0;
}

UserFlash user_flash_device() {
    const uintptr_t image_end = (uintptr_t)&__flash_binary_end - XIP_BASE;
    if (image_end > kRegionOffset) return UserFlash{ nullptr, write_sector };
    return UserFlash{ (const uint8_t *)(XIP_BASE + kRegionOffset), write_sector };
}
