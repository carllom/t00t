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

add_executable(t00t
    src/main.cpp
    src/osc/sine.cpp
    src/envelope.cpp
    src/engines/${T00T_ENGINE}/audio_engine.cpp
    src/output.cpp
    src/controller.cpp
    src/voice_alloc.cpp
    src/samples.cpp
    src/midi/midi_controller.cpp
    src/midi/usb_midi.cpp
    src/midi/uart_midi.cpp
    src/midi/usb_descriptors.c
    src/wslcd/lcd_st7789.cpp
    src/wslcd/gfx.cpp
    src/wslcd/display.cpp
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
