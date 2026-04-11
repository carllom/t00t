cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

project(t00t C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(t00t
    src/main.cpp
    src/voice.cpp
    src/audio_engine.cpp
    src/output.cpp
    src/controller.cpp
)

target_include_directories(t00t PRIVATE src)

target_link_libraries(t00t
    pico_stdlib
    pico_audio_i2s
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_timer
)

# USB serial for debug output, disable UART
pico_enable_stdio_usb(t00t 1)
pico_enable_stdio_uart(t00t 0)

# Generate .uf2 file for flashing
pico_add_extra_outputs(t00t)
