BUILD_DIR = build
UF2 = $(BUILD_DIR)/t00t.uf2

.PHONY: all clean flash

all: $(UF2)

$(UF2): CMakeLists.txt src/main.cpp
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DPICO_BOARD=pico ..
	$(MAKE) -C $(BUILD_DIR) -j$(shell nproc)

clean:
	rm -rf $(BUILD_DIR)

flash: $(UF2)
	@echo "Copy $(UF2) to the Pico (mount it in BOOTSEL mode):"
	@echo "  cp $(UF2) /media/$$USER/RPI-RP2/"
