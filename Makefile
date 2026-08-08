BUILD_DIR = build
UF2 = $(BUILD_DIR)/t00t.uf2

# Board selection: breadboard_rp2350 (default) or vgaboard_rp2350
BOARD ?= breadboard_rp2350

# Synthesis engine: subtractive (default) or groovebox
ENGINE ?= subtractive

# MIDI transport overrides: 0, 1, or "default" (use the board header's default).
#   make MIDI_USB=0          # DIN-only firmware
#   make MIDI_UART=0         # USB-only firmware
MIDI_USB  ?= default
MIDI_UART ?= default

# DMA/mixer buffer size in stereo frames: 256 (default) or 512.
#   make ENGINE=tracker DMA_BUFFER_SIZE=512
DMA_BUFFER_SIZE ?= default

# #31 P2 profiling rig: replaces the speech engine's normal MIDI-driven render
# loop with a self-cycling, pin-only measurement build (see engine.md "Speech
# Engine P2 profiling"). No effect on other engines.
#   make ENGINE=speech SPEECH_PROFILE=1
SPEECH_PROFILE ?= 0

# #42 P0 rig: replaces the FM engine's normal test-tone build with the
# stripped N-voice x 6-operator mixer (src/engines/fm/rig.h). No effect on
# other engines. See engine.md "FM P0 Rig (#42)".
#   make ENGINE=fm FM_PROFILE=1
FM_PROFILE ?= 0

# fm.md §3.6 tuning levers, each a compile-time switch on the #42 rig (only
# meaningful with FM_PROFILE=1) — "default" leaves rig.h's own #ifndef
# default in place, same sentinel convention as MIDI_USB/DMA_BUFFER_SIZE above.
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_VOICES=32
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_BLOCK=32
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_TABLE_BITS=10       # 1024-entry table
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_INTERLEAVE=1        # interleave op0/op1
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_NOT_IN_FLASH=1      # kernels in SRAM (noinline)
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_NOT_IN_FLASH=2      # kernels noinline, still flash (control)
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_SMULWB=1             # M33 smulwb fusion
#   make ENGINE=fm FM_PROFILE=1 FM_RIG_FB=0                 # op3 plain, not self-feedback
FM_RIG_VOICES      ?= default
FM_RIG_BLOCK        ?= default
FM_RIG_TABLE_BITS   ?= default
FM_RIG_INTERLEAVE   ?= default
FM_RIG_NOT_IN_FLASH ?= default
FM_RIG_SMULWB       ?= default
FM_RIG_FB           ?= default

CMAKE_FLAGS = -DPICO_BOARD=$(BOARD) -DPICO_PLATFORM=rp2350 \
              -DMIDI_USB=$(MIDI_USB) -DMIDI_UART=$(MIDI_UART) \
              -DT00T_ENGINE=$(ENGINE) -DDMA_BUFFER_SIZE=$(DMA_BUFFER_SIZE) \
              -DSPEECH_PROFILE=$(SPEECH_PROFILE) -DFM_PROFILE=$(FM_PROFILE) \
              -DFM_RIG_VOICES=$(FM_RIG_VOICES) -DFM_RIG_BLOCK=$(FM_RIG_BLOCK) \
              -DFM_RIG_TABLE_BITS=$(FM_RIG_TABLE_BITS) -DFM_RIG_INTERLEAVE=$(FM_RIG_INTERLEAVE) \
              -DFM_RIG_NOT_IN_FLASH=$(FM_RIG_NOT_IN_FLASH) -DFM_RIG_SMULWB=$(FM_RIG_SMULWB) \
              -DFM_RIG_FB=$(FM_RIG_FB)

HOST_BUILD_DIR = tools/host_render/build

.PHONY: all clean flash host host-clean

# Always (re)configure — cmake is a no-op when nothing changed, and this
# ensures BOARD / MIDI_* changes take effect. The inner make is incremental.
all:
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) ..
	$(MAKE) -C $(BUILD_DIR) -j$(shell nproc)

clean:
	rm -rf $(BUILD_DIR)

flash: all
	@echo "Copy $(UF2) to the Pico (mount it in BOOTSEL mode):"
	@echo "  cp $(UF2) /media/$$USER/RPI-RP2/"

# Host-side DSP tooling (tools/host_render): builds common-layer headers with
# the host compiler and renders them to WAV for verification off-device. Does
# not touch pico-sdk or the device build above.
host:
	@mkdir -p $(HOST_BUILD_DIR)
	cd $(HOST_BUILD_DIR) && cmake ..
	$(MAKE) -C $(HOST_BUILD_DIR) -j$(shell nproc)

host-clean:
	rm -rf $(HOST_BUILD_DIR)
