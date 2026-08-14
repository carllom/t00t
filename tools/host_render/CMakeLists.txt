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

# Host build of src/engines/speech/render.h (#27): the speech engine
# skeleton's shared device/host test-tone renderer, proving the ZOH x2
# native-rate resample seam before any formant DSP exists.
add_executable(render_speech
    render_speech.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../src/osc/sine.cpp
)
target_include_directories(render_speech PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# Host build of src/engines/fm/render.h (#41): the FM engine skeleton's
# shared device/host test-tone renderer, proving the FM-specific 4096-entry
# sine table (sine_tab.h, phase >> 20, no interpolation) before any operator
# kernel exists. Pulls in osc/sine.cpp only for pan_gains_q15's wavetable
# (pan.h) -- fm_sine_table itself is a header-only inline variable, no extra
# source needed (sine_tab.h).
add_executable(render_fm
    render_fm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../src/osc/sine.cpp
)
target_include_directories(render_fm PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# #47 P3 patch bank: patches.h is generated locally by tools/syx2patch.py
# and gitignored (see the top-level CMakeLists.txt's matching gate) --
# render_fm.cpp's patch-bank render only compiles in when it's present.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../src/engines/fm/patches.h)
    target_compile_definitions(render_fm PRIVATE T00T_FM_HAS_PATCHES=1)
endif()

# Host build of src/engines/fm/rig.h (#42): the P0 measurement rig's
# stripped 6-operator x N-voice mixer, checked here for correctness (real,
# finite, non-clipping audio through all three kernel variants) before the
# actual cycle-count bench session. Fully self-contained -- rig.h has its own
# table and doesn't touch osc/sine.h or pan.h -- so no extra source needed.
add_executable(render_fm_rig render_fm_rig.cpp)
target_include_directories(render_fm_rig PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# CHIP_STRICT reference-diff harness (module_chip.md §11.1, P0/P1). Takes the same
# CLI as tools/sid_ref/resid_render.cpp; tools/sid_compare.py runs both and
# scores the difference. Header-only against src/chip/, no extra .cpp inputs.
add_executable(render_sid render_sid.cpp)
target_include_directories(render_sid PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
    ${CMAKE_CURRENT_LIST_DIR}/../sid_ref
)

# Control-plane counterpart to render_sid, above (module_chip.md §11.1's other half):
# exact CSV dumps of src/chip/'s primitives, diffed by tools/sid_ctl_diff.py
# against tools/sid_ref/resid_dump.cpp's reference dumps. Header-only.
add_executable(t00t_chip_dump t00t_chip_dump.cpp)
target_include_directories(t00t_chip_dump PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

# AY_STRICT reference-diff harness (module_chip.md §12.1, AY-P0) -- the second chip in
# this module, same shape as render_sid above. Takes the same CLI as
# tools/ay_ref/ayumi_render.cpp; tools/sid_compare.py (reused unmodified,
# it is generic over any two float32 WAVs) scores the difference.
add_executable(render_ay render_ay.cpp)
target_include_directories(render_ay PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
    ${CMAKE_CURRENT_LIST_DIR}/../ay_ref
)

# Control-plane counterpart to render_ay, above: exact CSV dumps of
# src/chip/ay_osc.h / ay_envelope.h, diffed by tools/ay_ctl_diff.py against
# tools/ay_ref/ayumi_dump.cpp's reference dumps. Header-only.
add_executable(t00t_ay_dump t00t_ay_dump.cpp)
target_include_directories(t00t_ay_dump PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)
