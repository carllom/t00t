BUILD_DIR = build
UF2 = $(BUILD_DIR)/t00t.uf2

# Board selection: breadboard_rp2350 (default) or vgaboard_rp2350
BOARD ?= breadboard_rp2350

# Synthesis engine: subtractive (default), groovebox, tracker, speech or chip
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

# Chip module F0 measurement rig levers (src/engines/chip/rig.h, sid.md P0).
# Each measurement is its own build -- a runtime switch would put a branch
# inside the loop whose cycle count is the thing being measured. "default"
# leaves rig.h's own value in place.
#
#   make ENGINE=chip                                  # 24 voices, 12 filtered
#   make ENGINE=chip CHIP_RIG_FILTERED=0              # unfiltered, for the diff
#   make ENGINE=chip CHIP_RIG_MOD=1 CHIP_RIG_OVERSAMPLE=2   # sync at 2x
#   make ENGINE=chip CHIP_WAVE_DAC=0                  # without the 8 KB DAC LUT
CHIP_RIG_VOICES     ?= default
CHIP_RIG_BUSES      ?= default
CHIP_RIG_FILTERED   ?= default
CHIP_RIG_OVERSAMPLE ?= default
CHIP_RIG_SAT        ?= default
CHIP_RIG_SUBBLOCK   ?= default
CHIP_RIG_MOD        ?= default
CHIP_WAVE_DAC       ?= default

CMAKE_FLAGS = -DPICO_BOARD=$(BOARD) -DPICO_PLATFORM=rp2350 \
              -DMIDI_USB=$(MIDI_USB) -DMIDI_UART=$(MIDI_UART) \
              -DT00T_ENGINE=$(ENGINE) -DDMA_BUFFER_SIZE=$(DMA_BUFFER_SIZE) \
              -DSPEECH_PROFILE=$(SPEECH_PROFILE) \
              -DCHIP_RIG_VOICES=$(CHIP_RIG_VOICES) -DCHIP_RIG_BUSES=$(CHIP_RIG_BUSES) \
              -DCHIP_RIG_FILTERED=$(CHIP_RIG_FILTERED) \
              -DCHIP_RIG_OVERSAMPLE=$(CHIP_RIG_OVERSAMPLE) \
              -DCHIP_RIG_SAT=$(CHIP_RIG_SAT) -DCHIP_RIG_SUBBLOCK=$(CHIP_RIG_SUBBLOCK) \
              -DCHIP_RIG_MOD=$(CHIP_RIG_MOD) -DCHIP_WAVE_DAC=$(CHIP_WAVE_DAC)

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
