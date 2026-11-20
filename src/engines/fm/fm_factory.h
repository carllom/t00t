#pragma once

#include "fm_module.h"
#include "patch/platform.h"
#include "patch/stored_format.h"
#include <cstdint>

// The FM factory banks. The DX7 ROM banks are generated locally from .syx
// files (tools/syx2patch.py bank -> factory_banks.h, gitignored: the voice
// data is Yamaha's); without them the module has one built-in bank holding
// the INIT VOICE, so Program Change and the Multi always find a Patch.

// The INIT VOICE as a stored Patch: every parameter at its default.
struct FmStoredInit {
    uint8_t data[kPatchHeaderSize + FM_P_COUNT];
};

constexpr FmStoredInit fm_stored_init() {
    FmStoredInit s{};
    // The header as stored_write_header() lays it out.
    const char name[] = "INIT VOICE";
    for (uint32_t i = 0; i < 4; i++) s.data[i] = kPatchMagic[i];
    s.data[4] = (uint8_t)FmModule::kModuleId;
    s.data[5] = FmModule::kSchemaVersion;
    for (uint32_t i = 0; i < sizeof name - 1; i++) s.data[6 + i] = (uint8_t)name[i];
    s.data[kPatchHeaderSize - 2] = (uint8_t)(FM_P_COUNT & 0xFF);
    s.data[kPatchHeaderSize - 1] = (uint8_t)(FM_P_COUNT >> 8);
    for (uint32_t i = 0; i < FM_P_COUNT; i++) {
        const ParamDescriptor &d = FmModule::kDescriptors[i];
        s.data[kPatchHeaderSize + i] = (uint8_t)(d.def - d.min);
    }
    return s;
}
static_assert(patch_stored_size(FmModule::kDescriptors) == sizeof(FmStoredInit::data),
              "every FM parameter is stored in one byte");

inline constexpr FmStoredInit kFmStoredInit = fm_stored_init();

#ifdef T00T_FM_HAS_FACTORY_BANKS
#include "factory_banks.h"  // kFmFactoryBanks[], kFmFactoryBankCount
#else
inline const StoredPatchRef kFmBuiltinBank[] = { { kFmStoredInit.data, sizeof kFmStoredInit.data } };
inline const FactoryBank kFmFactoryBanks[] = { { kFmBuiltinBank, 1 } };
static constexpr uint32_t kFmFactoryBankCount = 1;
#endif
