#!/bin/bash
# usb_watch.sh — Continuously monitor USB device connections/disconnections
#
# Usage:
#   ./usb_watch.sh              # Watch all USB changes
#   ./usb_watch.sh 046D         # Watch only Logitech devices (VID=046D)
#   ./usb_watch.sh 045E         # Watch only Microsoft/Xbox devices (VID=045E)
#
# Useful for catching:
#   - G29 re-enumeration after mode switch
#   - Xbox device connection/rejection
#   - Auth board enumeration

FILTER="${1:-}"
INTERVAL=2

echo "👁  Watching USB devices (${INTERVAL}s interval)..."
if [ -n "$FILTER" ]; then
    echo "   Filter: VID/PID containing '$FILTER'"
fi
echo "   Press Ctrl-C to stop"
echo "   ─────────────────────────────────────"

# Get initial state
PREV=$(system_profiler SPUSBDataType 2>/dev/null)

while true; do
    sleep $INTERVAL
    CURR=$(system_profiler SPUSBDataType 2>/dev/null)

    if [ "$CURR" != "$PREV" ]; then
        TS=$(date +%H:%M:%S)

        # Find new devices (lines in CURR not in PREV)
        NEW=$(diff <(echo "$PREV") <(echo "$CURR") | grep "^>" | grep -v "^---" || true)
        GONE=$(diff <(echo "$PREV") <(echo "$CURR") | grep "^<" | grep -v "^---" || true)

        if [ -n "$FILTER" ]; then
            NEW=$(echo "$NEW" | grep -i "$FILTER" || true)
            GONE=$(echo "$GONE" | grep -i "$FILTER" || true)
        fi

        if [ -n "$NEW" ]; then
            echo "[$TS] ✅ USB CHANGE DETECTED:"
            echo "$NEW" | head -20
            echo ""
        fi
        if [ -n "$GONE" ]; then
            echo "[$TS] ❌ USB DEVICE REMOVED:"
            echo "$GONE" | head -20
            echo ""
        fi

        PREV="$CURR"
    fi
done
