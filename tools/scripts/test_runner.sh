#!/bin/bash
# test_runner.sh — Interactive hardware test runner
#
# Walks through the Phase 0 test matrix, records results with timestamps.
# Saves results to test_results/ directory.
#
# Usage:
#   ./test_runner.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/test_results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="$RESULTS_DIR/test_run_${TIMESTAMP}.md"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass_count=0
fail_count=0
skip_count=0

record() {
    echo "$1" >> "$RESULTS_FILE"
}

ask_result() {
    local test_id="$1"
    local test_name="$2"
    local notes=""

    echo ""
    echo -e "${YELLOW}━━━ $test_id: $test_name ━━━${NC}"
    echo ""
    read -p "Result [P]ass / [F]ail / [S]kip: " result
    case "${result^^}" in
        P|PASS)
            echo -e "${GREEN}✅ PASS${NC}"
            read -p "Notes (optional): " notes
            record "| $test_id | $test_name | **PASS** | $notes |"
            pass_count=$((pass_count + 1))
            ;;
        F|FAIL)
            echo -e "${RED}❌ FAIL${NC}"
            read -p "Error description: " notes
            record "| $test_id | $test_name | **FAIL** | $notes |"
            fail_count=$((fail_count + 1))
            ;;
        S|SKIP)
            echo -e "${YELLOW}⏭  SKIP${NC}"
            read -p "Reason: " notes
            record "| $test_id | $test_name | SKIP | $notes |"
            skip_count=$((skip_count + 1))
            ;;
        *)
            echo "Invalid. Marking as SKIP."
            record "| $test_id | $test_name | SKIP | Invalid input |"
            skip_count=$((skip_count + 1))
            ;;
    esac
}

# ── Header ──
echo ""
echo "╔═══════════════════════════════════════════════╗"
echo "║   G29-Xbox Bridge — Hardware Test Runner      ║"
echo "║   $(date +%Y-%m-%d\ %H:%M:%S)                          ║"
echo "╚═══════════════════════════════════════════════╝"
echo ""
echo "Results will be saved to: $RESULTS_FILE"

record "# Hardware Test Results"
record ""
record "**Date:** $(date +%Y-%m-%d\ %H:%M:%S)"
record "**Firmware:** $(cd "$PROJECT_ROOT" && git log --oneline -1)"
record ""
record "| Test | Description | Result | Notes |"
record "|------|-------------|--------|-------|"

# ── Tests ──

echo ""
echo "Ensure RP2040 #2 is flashed with firmware_g29.uf2"
echo "Ensure UART adapter is connected to GPIO 16"
read -p "Ready? [Enter to continue] "

ask_result "0a-1" "RP2040 #2 boot + UART output"

echo ""
echo "Now flash RP2040 #1 with firmware_xbox.uf2"
echo "Connect to Mac via USB-C"
read -p "Ready? [Enter to continue] "

ask_result "0a-2" "RP2040 #1 USB enumeration on Mac"

echo ""
echo "Connect two USB devices to the breakout boards"
echo "(Can be any two USB devices — keyboard+mouse works)"
read -p "Ready? [Enter to continue] "

ask_result "0b" "PIO dual root port — two devices enumerate"

echo ""
echo "Connect SPI wires between boards (SCK, MOSI, MISO, CS, GND)"
read -p "Ready? [Enter to continue] "

ask_result "0c" "SPI bus — 1kHz CRC-validated exchange (60s soak)"

echo ""
echo "Connect G29 wheel to USB-A breakout #1"
echo "Set G29 mode switch to PS3"
echo "Plug in G29's 12V power adapter"
read -p "Ready? [Enter to continue] "

ask_result "0d-1" "G29 initial USB enumeration"
ask_result "0d-2" "G29 mode switch + re-enumeration"
ask_result "0d-3" "G29 steering axis (full left/center/right)"
ask_result "0d-4" "G29 pedals (gas/brake/clutch)"
ask_result "0d-5" "G29 buttons (Cross, Circle, PS, D-pad)"

echo ""
echo "Connect PowerA auth board to USB-A breakout #2"
read -p "Ready? [Enter to continue] "

ask_result "0e" "Auth board USB enumeration"

echo ""
echo "Full chain: G29 + Auth + SPI + both boards"
read -p "Ready? [Enter to continue] "

ask_result "0f" "End-to-end: wheel input propagates through SPI"

# ── Summary ──
record ""
record "## Summary"
record ""
record "- **Passed:** $pass_count"
record "- **Failed:** $fail_count"
record "- **Skipped:** $skip_count"

echo ""
echo "╔═══════════════════════════════════════════════╗"
echo -e "║   Results: ${GREEN}$pass_count passed${NC}, ${RED}$fail_count failed${NC}, ${YELLOW}$skip_count skipped${NC}     ║"
echo "╚═══════════════════════════════════════════════╝"
echo ""
echo "📄 Saved to: $RESULTS_FILE"
