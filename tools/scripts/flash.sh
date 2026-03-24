#!/bin/bash
# flash.sh — Build and flash firmware to RP2040-Zero
#
# Usage:
#   ./flash.sh g29          # Build + flash firmware-g29
#   ./flash.sh xbox         # Build + flash firmware-xbox
#   ./flash.sh g29 --flash-only   # Skip build, just flash
#   ./flash.sh g29 --monitor      # Build, flash, then start UART monitor
#
# The script waits for the RPI-RP2 drive to appear (hold BOOT + plug in).

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
ARM_PATH="$HOME/arm-toolchain/bin"

export PICO_SDK_PATH
export PATH="$ARM_PATH:$PATH"

TARGET=""
FLASH_ONLY=false
MONITOR=false

# Parse args
for arg in "$@"; do
    case "$arg" in
        g29|G29)    TARGET="g29" ;;
        xbox|XBOX)  TARGET="xbox" ;;
        --flash-only) FLASH_ONLY=true ;;
        --monitor)    MONITOR=true ;;
        *)
            echo "Usage: $0 <g29|xbox> [--flash-only] [--monitor]"
            exit 1
            ;;
    esac
done

if [ -z "$TARGET" ]; then
    echo "Usage: $0 <g29|xbox> [--flash-only] [--monitor]"
    exit 1
fi

if [ "$TARGET" = "g29" ]; then
    FW_DIR="$PROJECT_ROOT/firmware-g29"
    UF2_NAME="firmware_g29.uf2"
elif [ "$TARGET" = "xbox" ]; then
    FW_DIR="$PROJECT_ROOT/firmware-xbox"
    UF2_NAME="firmware_xbox.uf2"
fi

UF2_PATH="$FW_DIR/build/$UF2_NAME"

# ── Build ──
if [ "$FLASH_ONLY" = false ]; then
    echo "🔨 Building $TARGET firmware..."
    mkdir -p "$FW_DIR/build"
    cd "$FW_DIR/build"

    if [ ! -f "Makefile" ]; then
        cmake .. 2>&1 | tail -3
    fi

    make -j8 2>&1 | tail -5

    if [ ! -f "$UF2_PATH" ]; then
        echo "❌ Build failed — $UF2_NAME not found"
        exit 1
    fi

    SIZE=$(ls -la "$UF2_PATH" | awk '{print $5}')
    echo "✅ Built: $UF2_NAME ($SIZE bytes)"
fi

# ── Flash ──
echo ""
echo "⏳ Waiting for RPI-RP2 drive..."
echo "   Hold BOOT button on the RP2040-Zero, then plug it into Mac via USB-C"

# Wait for drive to appear (timeout 30s)
TIMEOUT=30
ELAPSED=0
while [ ! -d "/Volumes/RPI-RP2" ]; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo "❌ Timeout — RPI-RP2 drive did not appear in ${TIMEOUT}s"
        echo "   Tips:"
        echo "   - Hold BOOT *before* plugging in"
        echo "   - Try a different USB-C cable (some are charge-only)"
        echo "   - Try a different USB port on the Mac"
        exit 1
    fi
    printf "."
done
echo ""

echo "📀 Flashing $UF2_NAME to /Volumes/RPI-RP2/..."
cp "$UF2_PATH" /Volumes/RPI-RP2/

# Wait for drive to disappear (board rebooted)
sleep 1
ELAPSED=0
while [ -d "/Volumes/RPI-RP2" ]; do
    sleep 0.5
    ELAPSED=$((ELAPSED + 1))
    if [ $ELAPSED -ge 10 ]; then
        echo "⚠️  Drive didn't disappear — board may not have rebooted"
        break
    fi
done

echo "✅ Flashed! Board should be running $TARGET firmware."

# ── Monitor ──
if [ "$MONITOR" = true ]; then
    echo ""
    sleep 2  # Wait for board to boot
    "$PROJECT_ROOT/tools/scripts/monitor.sh" --log
fi
