#pragma once

#include <cstdint>

// Identifies which module a stored Patch or Multi belongs to. Values are
// stored in patch headers and partition the user-bank flash region, so they
// are permanent: never renumber, only append.
enum class ModuleId : uint8_t {
    NONE        = 0,
    SUBTRACTIVE = 1,
    GROOVEBOX   = 2,
    FM          = 3,
    OPL         = 4,
    CHIP        = 5,
    SPEECH      = 6,
    TRACKER     = 7,
};
