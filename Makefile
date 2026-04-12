BUILD_DIR = build
UF2 = $(BUILD_DIR)/t00t.uf2

# Board selection: vgaboard_rp2350 (default) or breadboard_rp2350
BOARD ?= vgaboard_rp2350

.PHONY: all clean flash

all: $(UF2)

SOURCES = $(wildcard src/*.cpp src/*.h)

$(UF2): CMakeLists.txt $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DPICO_BOARD=$(BOARD) -DPICO_PLATFORM=rp2350 ..
	$(MAKE) -C $(BUILD_DIR) -j$(shell nproc)

clean:
	rm -rf $(BUILD_DIR)

flash: $(UF2)
	@echo "Copy $(UF2) to the Pico (mount it in BOOTSEL mode):"
	@echo "  cp $(UF2) /media/$$USER/RPI-RP2/"
