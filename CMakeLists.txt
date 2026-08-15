cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

project(t00t C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Synthesis engine selection. Each engine lives in src/engines/<name>/ and
# provides its own engine.h (VoiceParams/ParamExchange), audio_engine.cpp
# (Core 1 render loop), and preset/kit tables. Shared enums and constants live
# in src/engine_base.h; the reusable DSP layer (osc/, filter.h, fx/) depends
# only on that. Override with -DT00T_ENGINE=<name>.
if(NOT DEFINED T00T_ENGINE)
    set(T00T_ENGINE "subtractive")
endif()
message(STATUS "T00T synthesis engine: ${T00T_ENGINE}")

# An engine may provide its own MIDI routing (fixed drum map, mono voices, …).
# If it does, use it; otherwise fall back to the shared subtractive controller.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/engines/${T00T_ENGINE}/midi_controller.cpp)
    set(ENGINE_MIDI_CONTROLLER src/engines/${T00T_ENGINE}/midi_controller.cpp)
else()
    set(ENGINE_MIDI_CONTROLLER src/midi/midi_controller.cpp)
endif()

# The VGA-board button controller (src/controller.cpp) is hardcoded to the
# subtractive engine's specific preset shape (VoicePreset, WAVE_SAMPLE,
# osc_sample_phase_inc) -- not a generic "any engine with a presets.h" demo,
# so it only builds for that engine. speech and groovebox target the
# buttonless breadboard (HAS_BUTTONS 0), so they were never meant to pull
# this in either, despite each having its own presets.h.
set(ENGINE_OPTIONAL_SOURCES "")
if(T00T_ENGINE STREQUAL "subtractive")
    list(APPEND ENGINE_OPTIONAL_SOURCES src/controller.cpp)
endif()

# The tracker engine has no dynamic voice allocation — channel N is voice N,
# fixed, per module_tracker.md's "Deviation From the Existing Layer Model" — so
# voice_alloc.cpp is left out of that build entirely (#13). main.cpp's calls
# into it are compiled out via HAS_VOICE_ALLOC below.
set(ENGINE_VOICE_ALLOC src/voice_alloc.cpp)
if(T00T_ENGINE STREQUAL "tracker")
    set(ENGINE_VOICE_ALLOC "")
endif()

# The tracker engine's Core 0 player task (#18): song load, TickBlock ring,
# transport. Only tracker links it; main.cpp's calls into it are compiled
# out for other engines via HAS_TRACKER_PLAYER below.
set(ENGINE_TRACKER_PLAYER "")
if(T00T_ENGINE STREQUAL "tracker")
    set(ENGINE_TRACKER_PLAYER src/engines/tracker/player_task.cpp)
endif()

add_executable(t00t
    src/main.cpp
    src/osc/sine.cpp
    src/envelope.cpp
    src/engines/${T00T_ENGINE}/audio_engine.cpp
    src/output.cpp
    ${ENGINE_VOICE_ALLOC}
    ${ENGINE_TRACKER_PLAYER}
    src/samples.cpp
    ${ENGINE_MIDI_CONTROLLER}
    ${ENGINE_OPTIONAL_SOURCES}
    src/midi/usb_midi.cpp
    src/midi/uart_midi.cpp
    src/midi/usb_descriptors.c
    src/wslcd/lcd_st7789.cpp
    src/wslcd/gfx.cpp
    src/engines/${T00T_ENGINE}/display.cpp
)

# The selected engine directory is on the include path so `#include "engine.h"`
# (and its preset/kit headers) resolve to that engine's copy.
target_include_directories(t00t PRIVATE src src/midi src/wslcd src/engines/${T00T_ENGINE})

# Chip-module rig levers (src/engines/chip/rig.h). Each is a separate build
# because the value being measured is cycles per frame and a runtime switch
# would put a branch inside the loop under test. Passed through from the
# Makefile every configure, so nothing gets stuck in the CMake cache.
foreach(_rig CHIP_RIG_VOICES CHIP_RIG_BUSES CHIP_RIG_FILTERED CHIP_RIG_OVERSAMPLE
             CHIP_RIG_SAT CHIP_RIG_SUBBLOCK CHIP_RIG_MOD CHIP_WAVE_DAC
             CHIP_RIG_FX CHIP_RIG_SPEAKER)
    if(DEFINED ${_rig} AND NOT ${_rig} STREQUAL "default")
        target_compile_definitions(t00t PRIVATE ${_rig}=${${_rig}})
    endif()
endforeach()

# Optional MIDI transport overrides. Each may be 0, 1, or unset/"default"
# (board header decides). Passed through from the Makefile every configure,
# so values never get stuck in the CMake cache.
foreach(_midi MIDI_USB MIDI_UART)
    if(DEFINED ${_midi} AND NOT ${_midi} STREQUAL "default")
        target_compile_definitions(t00t PRIVATE ${_midi}=${${_midi}})
    endif()
endforeach()

# DMA/mixer buffer size override (#16). Unset/"default" keeps
# audio_common.h's built-in 256.
if(DEFINED DMA_BUFFER_SIZE AND NOT DMA_BUFFER_SIZE STREQUAL "default")
    target_compile_definitions(t00t PRIVATE T00T_SAMPLES_PER_BUFFER=${DMA_BUFFER_SIZE})
endif()

# #31 profiling rig override (speech engine only, see engine.h/audio_engine.cpp).
if(DEFINED SPEECH_PROFILE AND NOT SPEECH_PROFILE STREQUAL "0")
    target_compile_definitions(t00t PRIVATE T00T_SPEECH_PROFILE=1)
endif()

# #65 LPC corpus (speech engine only): lattice_words.h is generated locally
# by tools/talkie2lattice.py from real Talkie TMS5220 vocab source files and
# gitignored -- the corpus data has no clear redistribution license, same
# reasoning as #47's patches.h gate above. Guarded here so the build still
# succeeds (lattice.h's LATTICE_TEST_WORD only) when nobody has generated it
# locally yet.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/engines/speech/lattice_words.h)
    target_compile_definitions(t00t PRIVATE T00T_SPEECH_HAS_LATTICE_WORDS=1)
endif()

# #42 rig override (FM engine only, see src/engines/fm/rig.h/audio_engine.cpp).
if(DEFINED FM_PROFILE AND NOT FM_PROFILE STREQUAL "0")
    target_compile_definitions(t00t PRIVATE T00T_FM_PROFILE=1)
endif()

# module_fm.md §3.6 tuning levers on the #42 rig — each "default"/unset leaves
# rig.h's own #ifndef default in place, same convention as DMA_BUFFER_SIZE.
foreach(_lever FM_RIG_VOICES FM_RIG_BLOCK FM_RIG_TABLE_BITS FM_RIG_INTERLEAVE FM_RIG_NOT_IN_FLASH FM_RIG_SMULWB FM_RIG_FB)
    if(DEFINED ${_lever} AND NOT ${_lever} STREQUAL "default")
        target_compile_definitions(t00t PRIVATE ${_lever}=${${_lever}})
    endif()
endforeach()

# #45 BLOCK confirmation (FM engine's real EG control-rate block size, op.h's
# FM_BLOCK — distinct from FM_RIG_BLOCK above, which only affects the #42
# profiling rig). Unset/"default" leaves op.h's own #ifndef default (16) in
# place, same convention as DMA_BUFFER_SIZE.
if(DEFINED FM_BLOCK AND NOT FM_BLOCK STREQUAL "default")
    target_compile_definitions(t00t PRIVATE T00T_FM_BLOCK=${FM_BLOCK})
endif()

# #47 patch bank (FM engine only): patches.h is generated locally by
# tools/syx2patch.py from a real DX7 .syx dump and gitignored -- a real DX7
# bank's patch data is Yamaha's commercial sound-design work, not something
# to check into git history. Guarded here so the build still succeeds
# (FM_TEST_PATCH only) when nobody has generated it locally yet.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/engines/fm/patches.h)
    target_compile_definitions(t00t PRIVATE T00T_FM_HAS_PATCHES=1)
endif()

# Chip rig override (chip engine only, see src/engines/chip/audio_engine.cpp).
if(DEFINED CHIP_PROFILE AND NOT CHIP_PROFILE STREQUAL "0")
    target_compile_definitions(t00t PRIVATE T00T_CHIP_PROFILE=1)
endif()

# See ENGINE_VOICE_ALLOC above — tells main.cpp to skip the voice_alloc calls
# it would otherwise fail to link against on the tracker build.
if(T00T_ENGINE STREQUAL "tracker")
    target_compile_definitions(t00t PRIVATE HAS_VOICE_ALLOC=0)
    # See ENGINE_TRACKER_PLAYER above — tells main.cpp to call into
    # player_task.cpp's Core 0 song load / ring / transport hooks.
    target_compile_definitions(t00t PRIVATE HAS_TRACKER_PLAYER=1)
endif()

target_link_libraries(t00t
    pico_stdlib
    pico_audio_i2s
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_timer
    hardware_spi
    hardware_pwm
    tinyusb_device
    tinyusb_board
)

# USB is used for MIDI, not stdio
pico_enable_stdio_usb(t00t 0)
pico_enable_stdio_uart(t00t 0)

# Generate .uf2 file for flashing
pico_add_extra_outputs(t00t)
