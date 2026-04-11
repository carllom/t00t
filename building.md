# Building T00T

## Prerequisites

### Toolchain (Ubuntu/Debian)

```bash
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git
```

### Pico SDK and Extras

The Pico SDK and pico-extras are not included in the repository. Clone them into the project root:

```bash
git clone https://github.com/raspberrypi/pico-sdk.git --depth 1
cd pico-sdk && git submodule update --init && cd ..

git clone https://github.com/raspberrypi/pico-extras.git --depth 1
cd pico-extras && git submodule update --init && cd ..
```

This places `pico-sdk/` and `pico-extras/` alongside `CMakeLists.txt`, which is where the build expects them.

## Build

```bash
make
```

The firmware is written to `build/t00t.uf2`.

To start fresh:

```bash
make clean
```

## Flashing

1. Hold the BOOTSEL button on the Pico and plug it in via USB. It mounts as a USB mass storage device (typically at `/media/$USER/RPI-RP2/`).
2. Copy the firmware:

```bash
cp build/t00t.uf2 /media/$USER/RPI-RP2/
```

The Pico reboots and runs the firmware automatically.
