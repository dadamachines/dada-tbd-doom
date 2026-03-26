#!/bin/bash
# Flash Doom firmware + WAD to TBD-16 via CMSIS-DAP debug probe
#
# Usage:
#   ./flash.sh              # flash both firmware + WAD
#   ./flash.sh firmware     # flash firmware only
#   ./flash.sh wad          # flash WAD only
#   ./flash.sh uf2          # flash combined UF2 via USB (device must be in BOOTSEL mode)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/.pio/build/doom-tbd16"
WAD_FILE="$SCRIPT_DIR/data/doom1.whx"
FW_ELF="$BUILD_DIR/firmware.elf"
FW_UF2="$BUILD_DIR/firmware.uf2"
WAD_ADDR="0x10040000"

# PlatformIO's OpenOCD
OPENOCD="$HOME/.platformio/packages/tool-openocd-rp2040-earlephilhower/bin/openocd"
SCRIPTS="$HOME/.platformio/packages/tool-openocd-rp2040-earlephilhower/share/openocd/scripts"

OPENOCD_ARGS="-s $SCRIPTS -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c \"adapter speed 5000\""

flash_firmware() {
    if [ ! -f "$FW_ELF" ]; then
        echo "ERROR: $FW_ELF not found. Run 'pio run' first."
        exit 1
    fi
    echo "==> Flashing firmware..."
    eval "$OPENOCD" "$OPENOCD_ARGS" \
        -c '"init"' \
        -c '"reset halt"' \
        -c "\"flash write_image erase $FW_ELF\"" \
        -c '"reset run"' \
        -c '"shutdown"'
    echo "==> Firmware flashed OK"
}

flash_wad() {
    if [ ! -f "$WAD_FILE" ]; then
        echo "ERROR: $WAD_FILE not found."
        exit 1
    fi
    echo "==> Flashing WAD ($(du -h "$WAD_FILE" | cut -f1)) to $WAD_ADDR..."
    eval "$OPENOCD" "$OPENOCD_ARGS" \
        -c '"init"' \
        -c '"reset halt"' \
        -c "\"flash write_image erase $WAD_FILE $WAD_ADDR\"" \
        -c '"reset run"' \
        -c '"shutdown"'
    echo "==> WAD flashed OK"
}

flash_uf2() {
    if [ ! -f "$FW_UF2" ]; then
        echo "ERROR: $FW_UF2 not found. Run 'pio run' first."
        exit 1
    fi
    # Use picotool to flash — bypasses partition restrictions that block
    # the raw UF2 mass-storage copy for large firmware+WAD images.
    PICOTOOL="$HOME/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool"
    if [ ! -x "$PICOTOOL" ]; then
        echo "ERROR: picotool not found at $PICOTOOL"
        exit 1
    fi
    echo "==> Flashing combined UF2 via picotool ($(du -h "$FW_UF2" | cut -f1))..."
    "$PICOTOOL" load --ignore-partitions -v -x "$FW_UF2"
    echo "==> Done. Device rebooted."
}

case "${1:-all}" in
    firmware|fw)
        flash_firmware
        ;;
    wad|data)
        flash_wad
        ;;
    all|both)
        flash_firmware
        flash_wad
        ;;
    uf2)
        flash_uf2
        ;;
    *)
        echo "Usage: $0 [firmware|wad|all|uf2]"
        echo "  firmware  - flash firmware only via debug probe"
        echo "  wad       - flash WAD data only via debug probe"
        echo "  all       - flash firmware + WAD via debug probe (default)"
        echo "  uf2       - copy combined UF2 to RPI-RP2 USB drive"
        exit 1
        ;;
esac
