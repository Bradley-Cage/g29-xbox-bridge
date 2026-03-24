#!/usr/bin/env python3
"""
analyze_log.py — UART Log Analyzer

Scans a saved UART log file for errors, warnings, timing anomalies,
SPI CRC failures, USB events, and VM overruns. Produces a summary.

Usage:
    python3 analyze_log.py logs/uart_20260324_143000.log
    python3 analyze_log.py logs/uart_*.log    # Analyze multiple logs
"""

import sys
import re
from collections import Counter


def analyze_log(filepath):
    """Analyze a single UART log file."""
    print(f"\n{'='*60}")
    print(f"Analyzing: {filepath}")
    print(f"{'='*60}")

    errors = []
    warnings = []
    usb_events = []
    spi_events = []
    status_lines = []
    vm_events = []
    boot_count = 0
    line_count = 0

    try:
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip()
                line_count += 1

                # Count boots (watchdog resets show as repeated banners)
                if 'G29 Bridge' in line and '===' in line:
                    boot_count += 1

                # Errors
                if 'ERROR' in line or 'FAIL' in line or 'FAULT' in line:
                    errors.append(line)

                # Warnings
                if 'WARN' in line or 'timeout' in line.lower():
                    warnings.append(line)

                # USB events
                if '[USB]' in line or '[G29]' in line or '[AUTH]' in line or '[HID]' in line:
                    usb_events.append(line)

                # SPI events
                if '[SPI]' in line or 'CRC' in line.upper():
                    spi_events.append(line)

                # Status lines
                if '[STATUS]' in line:
                    status_lines.append(line)

                # VM events
                if '[VM]' in line or 'overrun' in line.lower() or 'equation' in line.lower():
                    vm_events.append(line)

    except FileNotFoundError:
        print(f"  File not found: {filepath}")
        return

    # ── Summary ──
    print(f"\n  Lines: {line_count}")
    print(f"  Boot count: {boot_count} {'(watchdog resets!)' if boot_count > 1 else '(normal)'}")

    # Errors
    if errors:
        print(f"\n  ❌ ERRORS ({len(errors)}):")
        for e in errors[:20]:
            print(f"    {e}")
        if len(errors) > 20:
            print(f"    ... and {len(errors) - 20} more")
    else:
        print(f"\n  ✅ No errors found")

    # Warnings
    if warnings:
        print(f"\n  ⚠️  WARNINGS ({len(warnings)}):")
        for w in warnings[:10]:
            print(f"    {w}")

    # USB Events
    if usb_events:
        print(f"\n  🔌 USB Events ({len(usb_events)}):")
        # Show unique events
        unique = list(dict.fromkeys(usb_events))
        for u in unique[:15]:
            count = usb_events.count(u)
            suffix = f" (x{count})" if count > 1 else ""
            print(f"    {u}{suffix}")

    # SPI
    if spi_events:
        print(f"\n  📡 SPI Events ({len(spi_events)}):")
        for s in spi_events[:10]:
            print(f"    {s}")

    # Status sampling
    if status_lines:
        print(f"\n  📊 Status samples ({len(status_lines)} total):")
        # Show first and last
        print(f"    First: {status_lines[0]}")
        print(f"    Last:  {status_lines[-1]}")

        # Parse G29 connection status
        g29_ok = sum(1 for s in status_lines if 'G29:OK' in s)
        g29_off = len(status_lines) - g29_ok
        print(f"    G29 connected: {g29_ok}/{len(status_lines)} ({100*g29_ok//max(len(status_lines),1)}%)")

        # Parse steering range
        steer_values = []
        for s in status_lines:
            m = re.search(r'Steer=([0-9A-Fa-f]{4})', s)
            if m:
                steer_values.append(int(m.group(1), 16))
        if steer_values:
            print(f"    Steering range: 0x{min(steer_values):04X} - 0x{max(steer_values):04X}")

    # VM
    if vm_events:
        print(f"\n  🧮 VM Events ({len(vm_events)}):")
        for v in vm_events[:10]:
            print(f"    {v}")

    print()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <log_file> [log_file2 ...]")
        print(f"       {sys.argv[0]} logs/uart_*.log")
        sys.exit(1)

    for filepath in sys.argv[1:]:
        analyze_log(filepath)


if __name__ == '__main__':
    main()
