#!/bin/bash
# monitor.sh — Auto-detect and connect to RP2040 debug output (USB CDC or UART)
#
# Usage:
#   ./monitor.sh              # Auto-detect serial port
#   ./monitor.sh /dev/tty.usbmodem101   # Specify port
#   ./monitor.sh --log        # Auto-detect + save to timestamped log file
#
# Board 2 (G29): debug output over USB CDC on the native USB-C port.
#   Shows up as /dev/tty.usbmodem* on macOS. Requires DTR assertion.
# Board 1 (XBOX): if UART adapter is connected, shows as /dev/tty.usbserial*.
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
        *) echo "Usage: $0 [--log] [/dev/tty.usbmodemXXXX | /dev/tty.usbserialXXXX]"; exit 1 ;;
    esac
done

# Auto-detect serial port
if [ -z "$PORT" ]; then
    echo "Scanning for serial ports..."
    PORTS=($(ls /dev/tty.usbmodem* /dev/tty.usbserial-* /dev/tty.SLAB_USBtoUART* 2>/dev/null || true))

    if [ ${#PORTS[@]} -eq 0 ]; then
        echo "No serial ports found."
        echo "  Board 2 (G29): plug in via USB-C — debug appears as /dev/tty.usbmodem*"
        echo "  Board 1 (XBOX): connect USB-TTL adapter if needed"
        echo "  Then run this script again."
        exit 1
    elif [ ${#PORTS[@]} -eq 1 ]; then
        PORT="${PORTS[0]}"
        echo "Found: $PORT"
    else
        echo "Multiple ports found:"
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
    echo "Logging to: $LOG_FILE"
fi

echo "Connecting to $PORT at ${BAUD} baud..."
echo "   Press Ctrl-C to exit"
echo "   ---"

# Use Python with pyserial — properly asserts DTR for macOS CDC
if python3 -c "import serial" 2>/dev/null; then
    if [ -n "$LOG_FILE" ]; then
        python3 -u -c "
import serial, sys, signal, datetime
signal.signal(signal.SIGINT, lambda *a: sys.exit(0))
s = serial.Serial('$PORT', $BAUD, timeout=0.5, dsrdtr=True)
s.dtr = True
log = open('$LOG_FILE', 'a')
try:
    while True:
        line = s.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
            out = f'[{ts}] {text}'
            print(out, flush=True)
            log.write(out + '\n')
            log.flush()
except SystemExit:
    pass
finally:
    log.close()
    s.close()
"
    else
        python3 -u -c "
import serial, sys, signal, datetime
signal.signal(signal.SIGINT, lambda *a: sys.exit(0))
s = serial.Serial('$PORT', $BAUD, timeout=0.5, dsrdtr=True)
s.dtr = True
try:
    while True:
        line = s.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
            print(f'[{ts}] {text}', flush=True)
except SystemExit:
    pass
finally:
    s.close()
"
    fi
# Fallback: picocom (sets DTR by default)
elif command -v picocom &>/dev/null; then
    if [ -n "$LOG_FILE" ]; then
        picocom -b $BAUD --logfile "$LOG_FILE" "$PORT"
    else
        picocom -b $BAUD "$PORT"
    fi
else
    echo "ERROR: Need pyserial or picocom for CDC debug."
    echo "  Install: pip3 install pyserial"
    exit 1
fi
