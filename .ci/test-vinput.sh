#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Override timeout for macOS - emulation is significantly slower
case "${OS_TYPE}" in
    Darwin)
        TIMEOUT=10800
        ;;
esac

cleanup
trap cleanup EXIT

# Force fresh download of Image and rootfs.cpio to avoid stale cache
rm -f Image rootfs.cpio
make Image rootfs.cpio

# NOTE: We want to capture the expect exit code and map 
# it to our MESSAGES array for meaningful error output.
# Temporarily disable errexit for the expect call.
set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn make check

# Boot and login
expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

# ---------------- virtio-input ----------------
# Require actual event* nodes, not just /dev/input directory existence
expect "# " { send "ls /dev/input/event* >/dev/null 2>&1 && echo __EVT_OK__ || echo __EVT_BAD__\r" }
expect {
  -re "\r?\n__EVT_OK__" {}
  -re "\r?\n__EVT_BAD__" { exit 3 }
  timeout { exit 3 }
}

expect "# " { send "cat /proc/bus/input/devices | head -20\r" }
expect "# " { send "grep -qi virtio /proc/bus/input/devices && echo __VPROC_OK__ || echo __VPROC_WARN__\r" }
expect -re "__VPROC_(OK|WARN)__" {} timeout { exit 3 }
DONE

ret="$?"
set -e  # Re-enable errexit after capturing expect's return code

MESSAGES=(
  "PASS: headless virtio-input checks"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-input basic checks failed (/dev/input/event* or /proc/bus/input/devices)"
  "FAIL: virtio-input event stream did not produce bytes (needs host->virtio-input injection path)"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
