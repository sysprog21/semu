#!/usr/bin/env bash
# Common functions and variables for semu CI scripts

set -euo pipefail

# Detect platform
MACHINE_TYPE="$(uname -m)"
OS_TYPE="$(uname -s)"

# Cleanup function - kills all semu processes
cleanup() {
    sleep 1
    pkill -9 semu 2>/dev/null || true
}

# Register cleanup on exit
trap cleanup EXIT INT TERM

# ASSERT function - executes command and exits on failure
ASSERT() {
    local exit_code

    set +e
    "$@"
    exit_code=$?
    set -e

    if [ "$exit_code" -ne 0 ]; then
        echo "Assert failed: $*" >&2
        exit "$exit_code"
    fi
}

# Determine timeout based on platform
# macOS needs more time to boot compared to Linux, so the timeout is set to
# 600 seconds for macOS to handle the longer startup. For Linux, 90 seconds
# is sufficient due to its faster boot process.
get_timeout() {
    local default_timeout="${SEMU_TEST_TIMEOUT:-}"

    if [ -n "$default_timeout" ]; then
        echo "$default_timeout"
        return
    fi

    case "${OS_TYPE}" in
        Darwin)
            echo "900"
            ;;
        Linux)
            echo "90"
            ;;
        *)
            echo "90"
            ;;
    esac
}

# Export TIMEOUT for use in scripts
TIMEOUT="$(get_timeout)"
export TIMEOUT

# Color codes for output
COLOR_GREEN='\e[32;01m'
COLOR_RED='\e[31;01m'
COLOR_RESET='\e[0m'

# Print success message
print_success() {
    printf "\n[ ${COLOR_GREEN}%s${COLOR_RESET} ]\n" "$1"
}

# Print error message
print_error() {
    printf "\n[ ${COLOR_RED}%s${COLOR_RESET} ]\n" "$1" >&2
}
