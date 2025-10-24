#!/bin/bash
# Verify that DTB CPU count matches expected count

DTB_FILE="${1}"
EXPECTED_COUNT="${2}"

if [ -z "$DTB_FILE" ] || [ -z "$EXPECTED_COUNT" ]; then
    echo "Usage: $0 <dtb_file> <expected_cpu_count>"
    exit 1
fi

if [ ! -f "$DTB_FILE" ]; then
    echo "Error: DTB file '$DTB_FILE' not found"
    exit 1
fi

# Count CPUs in DTB using dtc
DTC=$(which dtc 2>/dev/null)
if [ -z "$DTC" ]; then
    echo "Error: dtc tool not found in PATH"
    echo "DTB verification requires the device tree compiler (dtc)"
    echo "Please install dtc and try again"
    exit 1
fi

CPU_COUNT=$($DTC -I dtb -O dts "$DTB_FILE" 2>/dev/null | grep -c "cpu@")

if [ "$CPU_COUNT" -ne "$EXPECTED_COUNT" ]; then
    echo "========================================="
    echo "DTB Configuration Mismatch Detected!"
    echo "========================================="
    echo "DTB file '$DTB_FILE' contains $CPU_COUNT CPU(s)"
    echo "But you requested $EXPECTED_COUNT CPU(s)"
    echo ""
    echo "Solution:"
    echo "  make SMP=$EXPECTED_COUNT riscv-harts.dtsi minimal.dtb"
    echo ""
    echo "This will regenerate the DTB with correct CPU count."
    echo "========================================="
    exit 1
fi

exit 0
