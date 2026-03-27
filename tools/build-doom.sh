#!/bin/bash
# Build Doom for TBD-16
# Prerequisites:
#   - Pico SDK 2.0+ (with RP2350 support)
#   - pico-extras
#   - arm-none-eabi-gcc toolchain
#   - cmake

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DOOM_DIR="$SCRIPT_DIR/lib/rp2040-doom"

# Use PlatformIO's Pico SDK (already installed)
PIO_SDK="$HOME/.platformio/packages/framework-picosdk"
SDK_DIR="${PICO_SDK_PATH:-$PIO_SDK}"
EXTRAS_DIR="$SCRIPT_DIR/.pico-extras"

# Use PlatformIO's ARM toolchain
PIO_TC="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin"
if [ -d "$PIO_TC" ]; then
    export PATH="$PIO_TC:$PATH"
fi

# Check / install Pico SDK
if [ ! -d "$SDK_DIR" ]; then
    echo "==> Pico SDK not found at $SDK_DIR"
    echo "Install via PlatformIO or set PICO_SDK_PATH"
    exit 1
fi

# Clone pico-extras if needed (required by rp2040-doom CMake)
if [ ! -d "$EXTRAS_DIR" ]; then
    echo "==> Cloning pico-extras..."
    git clone --depth 1 https://github.com/raspberrypi/pico-extras.git "$EXTRAS_DIR"
fi

# Check toolchain
if ! command -v arm-none-eabi-gcc &> /dev/null; then
    echo "ERROR: arm-none-eabi-gcc not found in PATH."
    echo "Install with: brew install --cask gcc-arm-embedded"
    echo "Or ensure PlatformIO toolchain is at: $PIO_TC"
    exit 1
fi

echo "Using SDK: $SDK_DIR"
echo "Using GCC: $(which arm-none-eabi-gcc)"

# Initialize doom submodule if needed
cd "$DOOM_DIR"
git submodule update --init --recursive 2>/dev/null || true

# Configure with CMake
echo "==> Configuring for TBD-16..."
cmake --preset tbd16 \
    -DPICO_SDK_PATH="$SDK_DIR" \
    -DPICO_EXTRAS_PATH="$EXTRAS_DIR" \
    -DPICO_PLATFORM=rp2350-arm-s

# Build
echo "==> Building..."
cmake --build tbd16-build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

echo "==> Build complete!"
echo "Output: $DOOM_DIR/tbd16-build/src/doom_tiny.uf2"
