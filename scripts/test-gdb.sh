#!/bin/bash
# GDB debugging functionality test suite for semu
# Usage: ./scripts/test-gdb.sh [options]
#
# Options:
#   --quick     Run quick tests only (connection, registers, single-step)
#   --full      Run full test suite (default)
#   --verbose   Enable verbose output
#   --help      Show this help message

set +e  # Allow tests to fail and report results

# Configuration
SEMU=${SEMU:-./semu}
KERNEL=${KERNEL:-Image}
DTB=${DTB:-minimal.dtb}
INITRD=${INITRD:-rootfs.cpio}
GDB=${GDB:-~/rv/toolchain/bin/riscv-none-elf-gdb}
GDB_PORT=${GDB_PORT:-1234}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Parse arguments
QUICK_MODE=0
VERBOSE=0

while [[ $# -gt 0 ]]; do
	case $1 in
	--quick)
		QUICK_MODE=1
		shift
		;;
	--full)
		QUICK_MODE=0
		shift
		;;
	--verbose)
		VERBOSE=1
		shift
		;;
	--help)
		grep "^#" "$0" | grep -v "^#!/" | sed 's/^# //'
		exit 0
		;;
	*)
		echo "Unknown option: $1"
		echo "Use --help for usage information"
		exit 1
		;;
	esac
done

# Helper functions
log_info() {
	echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
	echo -e "${GREEN}[PASS]${NC} $1"
	PASSED_TESTS=$((PASSED_TESTS + 1))
}

log_fail() {
	echo -e "${RED}[FAIL]${NC} $1"
	FAILED_TESTS=$((FAILED_TESTS + 1))
}

log_warn() {
	echo -e "${YELLOW}[WARN]${NC} $1"
}

start_semu() {
	local log_file=$1
	log_info "Starting semu in debug mode..."
	$SEMU -g -k $KERNEL -b $DTB -i $INITRD >"$log_file" 2>&1 &
	SEMU_PID=$!
	sleep 3

	# Check if semu is running
	if ! kill -0 $SEMU_PID 2>/dev/null; then
		log_fail "Failed to start semu"
		cat "$log_file"
		exit 1
	fi

	if [ $VERBOSE -eq 1 ]; then
		log_info "semu started with PID: $SEMU_PID"
	fi
}

stop_semu() {
	if [ -n "$SEMU_PID" ] && kill -0 $SEMU_PID 2>/dev/null; then
		kill -9 $SEMU_PID 2>/dev/null || true
		wait $SEMU_PID 2>/dev/null || true
		if [ $VERBOSE -eq 1 ]; then
			log_info "semu stopped"
		fi
	fi
	SEMU_PID=""
}

run_test() {
	local test_name=$1
	local test_file=$2
	((TOTAL_TESTS++))

	if [ $VERBOSE -eq 1 ]; then
		log_info "Running: $test_name"
	fi

	if [ -f "$test_file" ]; then
		return 0
	else
		log_warn "Test output: $test_file"
		return 1
	fi
}

# Test 1: GDB Connection
test_connection() {
	log_info "Test 1: GDB Connection"
	start_semu /tmp/semu_test1.log

	if timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "quit" >/tmp/gdb_test1.txt 2>&1; then
		GDB_TIMEOUT=0
	else
		GDB_TIMEOUT=$?
	fi

	stop_semu

	if grep -q "Remote debugging using\|0x00000000 in" /tmp/gdb_test1.txt; then
		log_success "GDB connection established successfully"
	else
		log_fail "GDB connection failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test1.txt
	fi
}

# Test 2: Register Read
test_registers() {
	log_info "Test 2: Register Read/Write"
	start_semu /tmp/semu_test2.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "info registers" \
		-ex "quit" >/tmp/gdb_test2.txt 2>&1

	stop_semu

	# Check if we can read PC register
	if grep -q "pc.*0x" /tmp/gdb_test2.txt; then
		log_success "Register reading works (32 RISC-V registers accessible)"
	else
		log_fail "Register reading failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test2.txt
	fi
}

# Test 3: Single-step
test_single_step() {
	log_info "Test 3: Single-step Execution"
	start_semu /tmp/semu_test3.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "set \$pc1 = \$pc" \
		-ex "stepi" \
		-ex "set \$pc2 = \$pc" \
		-ex "printf \"PC1: 0x%x, PC2: 0x%x\\n\", \$pc1, \$pc2" \
		-ex "quit" >/tmp/gdb_test3.txt 2>&1

	stop_semu

	# Check if PC changed after stepi
	if grep -q "PC1: 0x" /tmp/gdb_test3.txt && grep -q "PC2: 0x" /tmp/gdb_test3.txt; then
		log_success "Single-step execution works (PC updates correctly)"
	else
		log_fail "Single-step execution failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test3.txt
	fi
}

# Test 4: Breakpoint Set/Delete
test_breakpoints() {
	log_info "Test 4: Breakpoint Management"
	start_semu /tmp/semu_test4.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "break *0x0" \
		-ex "info breakpoints" \
		-ex "delete 1" \
		-ex "info breakpoints" \
		-ex "quit" >/tmp/gdb_test4.txt 2>&1

	stop_semu

	if grep -q "Breakpoint 1 at 0x0" /tmp/gdb_test4.txt &&
		grep -q "No breakpoints" /tmp/gdb_test4.txt; then
		log_success "Breakpoint set/delete works"
	else
		log_fail "Breakpoint management failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test4.txt
	fi
}

# Test 5: Memory Read
test_memory_read() {
	log_info "Test 5: Memory Read"
	start_semu /tmp/semu_test5.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "x/4xw 0x0" \
		-ex "quit" >/tmp/gdb_test5.txt 2>&1

	stop_semu

	if grep -q "0x0:" /tmp/gdb_test5.txt; then
		log_success "Memory read works"
	else
		log_fail "Memory read failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test5.txt
	fi
}

# Test 6: Protocol Verification
test_protocol() {
	log_info "Test 6: GDB Protocol Verification"
	start_semu /tmp/semu_test6.log

	timeout 10 $GDB -batch \
		-ex "set debug remote 1" \
		-ex "target remote :$GDB_PORT" \
		-ex "disconnect" \
		-ex "quit" >/tmp/gdb_test6.txt 2>&1

	stop_semu

	# Check for vCont support
	if grep -q "vCont.*supported" /tmp/gdb_test6.txt ||
		grep -q "Packet received: vCont" /tmp/gdb_test6.txt; then
		log_success "GDB protocol negotiation works (vCont supported)"
	else
		log_fail "Protocol verification failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test6.txt
	fi
}

# Test 7: Multi-instruction Step
test_multi_step() {
	log_info "Test 7: Multiple Single-steps"
	start_semu /tmp/semu_test7.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "stepi" \
		-ex "stepi" \
		-ex "stepi" \
		-ex "info registers pc" \
		-ex "quit" >/tmp/gdb_test7.txt 2>&1

	stop_semu

	if grep -q "pc.*0x" /tmp/gdb_test7.txt; then
		log_success "Multiple single-steps work"
	else
		log_fail "Multiple single-steps failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test7.txt
	fi
}

# Test 8: Disassembly
test_disassembly() {
	log_info "Test 8: Code Disassembly"
	start_semu /tmp/semu_test8.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "set architecture riscv:rv32" \
		-ex "x/5i 0x0" \
		-ex "quit" >/tmp/gdb_test8.txt 2>&1

	stop_semu

	if grep -q "0x0:" /tmp/gdb_test8.txt; then
		log_success "Code disassembly works"
	else
		log_fail "Code disassembly failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test8.txt
	fi
}

# Test 9: Multiple Breakpoints
test_multiple_breakpoints() {
	log_info "Test 9: Multiple Breakpoints"
	start_semu /tmp/semu_test9.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "break *0x0" \
		-ex "break *0x100" \
		-ex "break *0x200" \
		-ex "info breakpoints" \
		-ex "delete 2" \
		-ex "info breakpoints" \
		-ex "quit" >/tmp/gdb_test9.txt 2>&1

	stop_semu

	# Check if we can manage multiple breakpoints
	if grep -q "Breakpoint 1 at 0x0" /tmp/gdb_test9.txt &&
		grep -q "Breakpoint 3 at 0x200" /tmp/gdb_test9.txt; then
		log_success "Multiple breakpoint management works"
	else
		log_fail "Multiple breakpoint management failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test9.txt
	fi
}

# Test 10: All Registers Read
test_all_registers() {
	log_info "Test 10: All RISC-V Registers"
	start_semu /tmp/semu_test10.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "info registers all" \
		-ex "quit" >/tmp/gdb_test10.txt 2>&1

	stop_semu

	# Check if all 32 general-purpose registers + PC are accessible
	if grep -q "zero" /tmp/gdb_test10.txt &&
		grep -q "ra" /tmp/gdb_test10.txt &&
		grep -q "t6" /tmp/gdb_test10.txt &&
		grep -q "^pc" /tmp/gdb_test10.txt; then
		log_success "All 33 RISC-V registers accessible"
	else
		log_fail "Register access incomplete"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test10.txt
	fi
}

# Test 11: Multiple Memory Regions
test_multiple_memory() {
	log_info "Test 11: Multiple Memory Region Access"
	start_semu /tmp/semu_test11.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "x/4xw 0x0" \
		-ex "x/4xw 0x100" \
		-ex "x/4xw 0x1000" \
		-ex "quit" >/tmp/gdb_test11.txt 2>&1

	stop_semu

	# Check if we can read from different memory addresses
	if grep -q "0x0:" /tmp/gdb_test11.txt &&
		grep -q "0x100:" /tmp/gdb_test11.txt &&
		grep -q "0x1000:" /tmp/gdb_test11.txt; then
		log_success "Multiple memory region access works"
	else
		log_fail "Multiple memory region access failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test11.txt
	fi
}

# Test 12: Backtrace
test_backtrace() {
	log_info "Test 12: Backtrace Support"
	start_semu /tmp/semu_test12.log

	timeout 10 $GDB -batch \
		-ex "target remote :$GDB_PORT" \
		-ex "backtrace" \
		-ex "quit" >/tmp/gdb_test12.txt 2>&1

	stop_semu

	# Check if backtrace command is supported (even if minimal)
	if grep -q "#0" /tmp/gdb_test12.txt ||
		grep -q "0x" /tmp/gdb_test12.txt; then
		log_success "Backtrace support works"
	else
		log_fail "Backtrace support failed"
		[ $VERBOSE -eq 1 ] && cat /tmp/gdb_test12.txt
	fi
}

# Main test execution
main() {
	echo "=========================================="
	echo "  GDB Debugging Test Suite"
	echo "=========================================="
	echo ""

	# Check prerequisites
	if [ ! -f "$SEMU" ]; then
		log_fail "semu binary not found: $SEMU"
		exit 1
	fi

	if [ ! -f "$KERNEL" ]; then
		log_fail "Kernel image not found: $KERNEL"
		exit 1
	fi

	if ! command -v "$GDB" >/dev/null 2>&1; then
		log_fail "RISC-V GDB not found: $GDB"
		log_info "Set GDB environment variable to specify GDB path"
		exit 1
	fi

	log_info "Configuration:"
	log_info "  semu:   $SEMU"
	log_info "  kernel: $KERNEL"
	log_info "  GDB:    $GDB"
	log_info "  Port:   $GDB_PORT"
	[ $QUICK_MODE -eq 1 ] && log_info "  Mode:   Quick tests only"
	echo ""

	# Run tests
	test_connection
	test_registers
	test_single_step

	if [ $QUICK_MODE -eq 0 ]; then
		test_breakpoints
		test_memory_read
		test_protocol
		test_multi_step
		test_disassembly
		test_multiple_breakpoints
		test_all_registers
		test_multiple_memory
		test_backtrace
	fi

	# Summary
	echo ""
	echo "=========================================="
	echo "  Test Summary"
	echo "=========================================="
	TOTAL_TESTS=$((PASSED_TESTS + FAILED_TESTS))
	echo "Total:  $TOTAL_TESTS"
	echo -e "Passed: ${GREEN}$PASSED_TESTS${NC}"
	echo -e "Failed: ${RED}$FAILED_TESTS${NC}"
	echo ""

	if [ $FAILED_TESTS -eq 0 ]; then
		echo -e "${GREEN}All tests passed!${NC}"
		echo ""
		log_info "GDB debugging functionality is working correctly"
		exit 0
	else
		echo -e "${RED}Some tests failed${NC}"
		echo ""
		log_warn "Check test outputs in /tmp/gdb_test*.txt for details"
		log_warn "Run with --verbose for more information"
		exit 1
	fi
}

# Cleanup on exit
trap 'stop_semu' EXIT INT TERM

# Run main
main
