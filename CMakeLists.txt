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

add_executable(t00t
    src/main.cpp
    src/osc/sine.cpp
    src/envelope.cpp
    src/audio_engine.cpp
    src/output.cpp
    src/controller.cpp
    src/voice_alloc.cpp
    src/samples.cpp
    src/midi/midi_controller.cpp
    src/midi/usb_midi.cpp
    src/midi/uart_midi.cpp
    src/midi/usb_descriptors.c
)

target_include_directories(t00t PRIVATE src src/midi)

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
    tinyusb_device
    tinyusb_board
)

# USB is used for MIDI, not stdio
pico_enable_stdio_usb(t00t 0)
pico_enable_stdio_uart(t00t 0)

# Generate .uf2 file for flashing
pico_add_extra_outputs(t00t)
