cmake_minimum_required(VERSION 3.13)
project(t00t_host_render CXX)

# Standalone host build — no pico-sdk, no cross toolchain. Compiles pure-DSP
# common-layer headers (src/res2p.h and friends) with the host compiler so
# they can be verified by rendering to WAV instead of on real hardware.
# See issue #5 phase 2.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(render_res2p render_res2p.cpp)
target_include_directories(render_res2p PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# Host build of src/engines/tracker/mixer.h (issue #15). Pulls in osc/sine.cpp
# for pan_gains_q15's wavetable; everything else it touches is header-only.
add_executable(render_tracker_mixer
    render_tracker_mixer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../src/osc/sine.cpp
)
target_include_directories(render_tracker_mixer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# Host build of src/engines/tracker/player.h (#17): the reference-diff
# harness's device-path renderer, plus player.h's own unit tests (no-arg
# invocation). See tools/host_render/diff_xm.py for the openmpt123 side.
add_executable(render_xm_device
    render_xm_device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../src/osc/sine.cpp
)
target_include_directories(render_xm_device PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)
