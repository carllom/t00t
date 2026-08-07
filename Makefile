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

CMAKE_FLAGS = -DPICO_BOARD=$(BOARD) -DPICO_PLATFORM=rp2350 \
              -DMIDI_USB=$(MIDI_USB) -DMIDI_UART=$(MIDI_UART) \
              -DT00T_ENGINE=$(ENGINE) -DDMA_BUFFER_SIZE=$(DMA_BUFFER_SIZE)

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
