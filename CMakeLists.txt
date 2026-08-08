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
# so it only builds for that engine. Was gated on "does presets.h exist"
# instead until #38 gave the speech engine its own presets.h (SpeechPreset,
# a different shape) and broke that heuristic -- speech targets the
# buttonless breadboard (HAS_BUTTONS 0) same as the groovebox, so it was
# never meant to pull this in either.
set(ENGINE_OPTIONAL_SOURCES "")
if(T00T_ENGINE STREQUAL "subtractive")
    list(APPEND ENGINE_OPTIONAL_SOURCES src/controller.cpp)
endif()

# The tracker engine has no dynamic voice allocation — channel N is voice N,
# fixed, per tracker.md's "Deviation From the Existing Layer Model" — so
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

# Optional MIDI transport overrides. Each may be 0, 1, or unset/"default"
# (board header decides). Passed through from the Makefile every configure,
# so values never get stuck in the CMake cache.
foreach(_midi MIDI_USB MIDI_UART)
    if(DEFINED ${_midi} AND NOT ${_midi} STREQUAL "default")
        target_compile_definitions(t00t PRIVATE ${_midi}=${${_midi}})
    endif()
endforeach()

# DMA/mixer buffer size override (#16: 256 vs 512 IRQ-overhead measurement).
# Unset/"default" keeps audio_common.h's built-in 256.
if(DEFINED DMA_BUFFER_SIZE AND NOT DMA_BUFFER_SIZE STREQUAL "default")
    target_compile_definitions(t00t PRIVATE T00T_SAMPLES_PER_BUFFER=${DMA_BUFFER_SIZE})
endif()

# #31 P2 profiling rig override (speech engine only, see engine.h/audio_engine.cpp).
if(DEFINED SPEECH_PROFILE AND NOT SPEECH_PROFILE STREQUAL "0")
    target_compile_definitions(t00t PRIVATE T00T_SPEECH_PROFILE=1)
endif()

# #42 P0 rig override (FM engine only, see src/engines/fm/rig.h/audio_engine.cpp).
if(DEFINED FM_PROFILE AND NOT FM_PROFILE STREQUAL "0")
    target_compile_definitions(t00t PRIVATE T00T_FM_PROFILE=1)
endif()

# fm.md §3.6 tuning levers on the #42 rig — each "default"/unset leaves
# rig.h's own #ifndef default in place, same convention as DMA_BUFFER_SIZE.
foreach(_lever FM_RIG_VOICES FM_RIG_BLOCK FM_RIG_TABLE_BITS FM_RIG_INTERLEAVE FM_RIG_NOT_IN_FLASH FM_RIG_SMULWB)
    if(DEFINED ${_lever} AND NOT ${_lever} STREQUAL "default")
        target_compile_definitions(t00t PRIVATE ${_lever}=${${_lever}})
    endif()
endforeach()

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
