#!/bin/bash
# monitor.sh — Auto-detect and connect to RP2040 UART debug output
#
# Usage:
#   ./monitor.sh              # Auto-detect serial port
#   ./monitor.sh /dev/tty.usbserial-1234   # Specify port
#   ./monitor.sh --log        # Auto-detect + save to timestamped log file
#
# Logs are saved to logs/ directory with timestamps.
# Press Ctrl-C to exit.

set -e

BAUD=115200
LOG_DIR="$(cd "$(dirname "$0")/../.." && pwd)/logs"
LOG_FILE=""
PORT=""

# Parse args
for arg in "$@"; do
    case "$arg" in
        --log) LOG_FILE="yes" ;;
        /dev/*) PORT="$arg" ;;
        *) echo "Usage: $0 [--log] [/dev/tty.usbserial-XXXX]"; exit 1 ;;
    esac
done

# Auto-detect serial port
if [ -z "$PORT" ]; then
    echo "🔍 Scanning for serial ports..."
    PORTS=($(ls /dev/tty.usbserial-* /dev/tty.usbmodem-* /dev/tty.SLAB_USBtoUART* 2>/dev/null || true))

    if [ ${#PORTS[@]} -eq 0 ]; then
        echo "❌ No serial ports found."
        echo "   Connect a USB-TTL adapter to RP2040 #2 GPIO 16"
        echo "   Then run this script again."
        exit 1
    elif [ ${#PORTS[@]} -eq 1 ]; then
        PORT="${PORTS[0]}"
        echo "✅ Found: $PORT"
    else
        echo "📋 Multiple ports found:"
        for i in "${!PORTS[@]}"; do
            echo "   [$i] ${PORTS[$i]}"
        done
        read -p "Select port [0]: " choice
        choice=${choice:-0}
        PORT="${PORTS[$choice]}"
    fi
fi

# Set up logging
if [ -n "$LOG_FILE" ]; then
    mkdir -p "$LOG_DIR"
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    LOG_FILE="$LOG_DIR/uart_${TIMESTAMP}.log"
    echo "📝 Logging to: $LOG_FILE"
fi

echo "📡 Connecting to $PORT at ${BAUD} baud..."
echo "   Press Ctrl-C to exit"
echo "   ─────────────────────────────────────"

# Use picocom if available (better than screen for scripting), fall back to cat
if command -v picocom &>/dev/null; then
    if [ -n "$LOG_FILE" ]; then
        picocom -b $BAUD --logfile "$LOG_FILE" "$PORT"
    else
        picocom -b $BAUD "$PORT"
    fi
elif command -v minicom &>/dev/null; then
    minicom -D "$PORT" -b $BAUD
else
    # Fallback: stty + cat (no interactive features, but works everywhere)
    stty -f "$PORT" $BAUD cs8 -cstopb -parenb raw -echo
    if [ -n "$LOG_FILE" ]; then
        # Tee to both terminal and log file with timestamps
        cat "$PORT" | while IFS= read -r line; do
            ts=$(date +%H:%M:%S.%3N)
            echo "[$ts] $line"
            echo "[$ts] $line" >> "$LOG_FILE"
        done
    else
        cat "$PORT" | while IFS= read -r line; do
            ts=$(date +%H:%M:%S.%3N)
            echo "[$ts] $line"
        done
    fi
fi
