#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Override timeout and sleep duration for macOS - emulation is significantly slower
case "${OS_TYPE}" in
    Darwin)
        TIMEOUT=10800
        DFB_SLEEP=180
        ;;
    *)
        DFB_SLEEP=5
        ;;
esac
export DFB_SLEEP

cleanup
trap cleanup EXIT

# Feature toggles are passed through environment variables, which do not
# participate in normal dependency tracking by 'make'. Force a rebuild here so
# one-feature-at-a-time test runs never reuse a stale 'semu' binary or DTB.
make -B semu minimal.dtb

# Force fresh guest artifacts so CI cannot reuse stale cached images.
rm -f Image rootfs.cpio ext4.img
make Image rootfs.cpio ext4.img

# NOTE: We want to capture the 'expect' exit code and map
# it to our 'MESSAGES' array for meaningful error output.
# Temporarily disable 'errexit' for the 'expect' call.
set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn make check

# Boot and login
expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

# ---------------- virtio-gpu basic checks ----------------
expect "# " { send "ls -la /dev/dri/ 2>/dev/null || true\r" }
# Emit a shell-expanded status marker so 'expect' cannot match the echoed command.
expect "# " { send "if test -c /dev/dri/card0; then status=OK; else status=MISSING; fi; printf \"__VGPU_DRM_%s__\\n\" \"\$status\"\r" } timeout { exit 3 }
expect {
  -exact "__VGPU_DRM_OK__" {}
  -exact "__VGPU_DRM_MISSING__" { exit 3 }
  timeout { exit 3 }
}

# virtio transport may be 'virtio-mmio', binding check should look at the
# 'virtio_gpu' driver directory.
expect "# " {
  send "sh -lc 'if ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf \"__VGPU_BIND_%s__\\n\" \"\u0024status\"'\r"
} timeout { exit 3 }
expect {
  -exact "__VGPU_BIND_OK__" {}
  -exact "__VGPU_BIND_BAD__" {
    send "ls -l /sys/bus/virtio/drivers/virtio_gpu/ 2>/dev/null || true\r"
    # Emit literal '$d' via '\u0024' to avoid Tcl variable substitution.
    send "sh -lc 'for d in /sys/bus/virtio/devices/virtio*; do echo \u0024d; ls -l \u0024d/driver 2>/dev/null || true; done'\r"
    exit 3
  }
  timeout { exit 3 }
}

# Useful logs (non-fatal)
expect "# " { send "dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|scanout|number of scanouts' | tail -n 80 || true\r" }

# ---------------- DirectFB2 ----------------
# Strategy:
# 1) Stop X11 if running (it holds the DRM device)
# 2) Check 'run.sh' exists at '/root/run.sh'
# 3) 'cd' to '/root' and source 'run.sh' to set 'PATH'/'LD_LIBRARY_PATH'
# 4) Verify 'df_drivertest' is in 'PATH'
# 5) Run 'df_drivertest' and check for DirectFB init messages
#
# NOTE: 'df_drivertest' may segfault when killed due to a race condition in
# DirectFB2's fusion module ('libfusion') during signal handling. When 'SIGTERM'
# is sent, the signal handler starts cleanup while the "Fusion Dispatch" thread
# may still be accessing shared state, leading to a use-after-free crash. The
# test passes if DirectFB init messages appear, even if the program crashes
# afterward during cleanup.

# Step 0: Stop X11 to release DRM device (it holds '/dev/dri/card0')
# Use 'pidof' with fallback to 'ps'/'grep' if 'pidof' is unavailable.
expect "# " {
  send "sh -lc '\
    if command -v pidof >/dev/null 2>&1; then \
      pidof Xorg >/dev/null 2>&1 && kill \u0024(pidof Xorg) 2>/dev/null || true; \
    else \
      ps | grep Xorg | grep -v grep | awk \"{print \u00241}\" | xargs kill 2>/dev/null || true; \
    fi; \
    sleep 1; printf \"__X11_%s__\\n\" STOPPED'\r"
}
expect {
  -exact "__X11_STOPPED__" {}
  timeout { exit 4 }
}

# Step 1: Check 'run.sh' exists.
expect "# " { send "if test -f /root/run.sh; then status=OK; else status=MISSING; fi; printf \"__RUNSH_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__RUNSH_OK__" {}
  -exact "__RUNSH_MISSING__" { exit 4 }
  timeout { exit 4 }
}

# Step 2: 'cd' to '/root' and source 'run.sh'.
expect "# " { send "if cd /root && . ./run.sh >/dev/null 2>&1; then status=DONE; else status=FAIL; fi; printf \"__SRC_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__SRC_DONE__" {}
  -exact "__SRC_FAIL__" { exit 4 }
  timeout { exit 4 }
}

# Step 3: Verify 'df_drivertest' is available.
expect "# " { send "if command -v df_drivertest >/dev/null 2>&1; then status=OK; else status=MISS; fi; printf \"__APP_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__APP_OK__" {}
  -exact "__APP_MISS__" { exit 4 }
  timeout { exit 4 }
}

# Step 4: Run 'df_drivertest' and check output (run in background, kill after
# delay).
expect "# " { send "df_drivertest >/tmp/dfb.log 2>&1 & sleep $env(DFB_SLEEP); kill \u0024! 2>/dev/null; head -30 /tmp/dfb.log\r" }
# Check for 'DRMKMS' init message.
expect "# " { send "if grep -qi 'DRMKMS/System' /tmp/dfb.log; then status=OK; else status=FAIL; fi; printf \"__DFB_%s__\\n\" \"\$status\"\r" }
expect {
  -exact "__DFB_OK__" {}
  -exact "__DFB_FAIL__" { exit 4 }
  timeout { exit 4 }
}
DONE

ret="$?"
set -e  # Re-enable 'errexit' after capturing 'expect' return code.

MESSAGES=(
  "PASS: headless virtio-gpu + DirectFB2 checks"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-gpu basic checks failed (/dev/dri/card0 or virtio_gpu binding)"
  "FAIL: DirectFB2 check failed (run.sh/df_drivertest missing or no DRMKMS init messages)"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
