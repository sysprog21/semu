#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Override timeout for macOS - emulation is significantly slower
case "${OS_TYPE}" in
    Darwin)
        TIMEOUT=1200
        ;;
esac

cleanup
trap cleanup EXIT

# Feature toggles are passed through environment variables, which do not
# participate in make's normal dependency tracking. Force a rebuild here so
# one-feature-at-a-time test runs never reuse a stale semu binary.
make -B semu minimal.dtb

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
# Require actual event* nodes, not just /dev/input directory existence.
# Print a concrete status marker that is not present in the echoed command text.
expect "# " { send "if ls /dev/input/event* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf \"__EVT_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__EVT_OK__" {}
  -exact "__EVT_BAD__" { exit 3 }
  timeout { exit 3 }
}

expect "# " { send "cat /proc/bus/input/devices | head -20\r" }
# Emit a shell-expanded status marker so expect cannot match the echoed command.
expect "# " { send "if grep -qi virtio /proc/bus/input/devices; then status=OK; else status=BAD; fi; printf \"__VPROC_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__VPROC_OK__" {}
  -exact "__VPROC_BAD__" { exit 3 }
  timeout { exit 3 }
}
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
