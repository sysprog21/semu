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

# Download pre-built ext4.img with DirectFB if not present
EXT4_URL="https://github.com/Mes0903/semu/raw/blob/ext4.img.bz2"
EXT4_SHA1="83ed49c16d341bdf3210141d5f6d5842b77a6adc"

if [ ! -f "ext4.img.bz2" ]; then
    echo "Downloading ext4.img.bz2 for DirectFB testing..."
    curl --progress-bar -O -L "${EXT4_URL}"
    echo "${EXT4_SHA1}  ext4.img.bz2" | shasum -a 1 -c -
fi

echo "Decompressing ext4.img.bz2 for DirectFB testing..."
rm -f ext4.img
bunzip2 -kf ext4.img.bz2

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

# ---------------- vgpu basic checks ----------------
expect "# " { send "ls -la /dev/dri/ 2>/dev/null || true\r" }
expect "# " { send "test -c /dev/dri/card0 && echo __VGPU_DRM_OK__ || echo __VGPU_DRM_MISSING__\r" } timeout { exit 3 }
expect {
  -re "\r?\n__VGPU_DRM_OK__" {}
  -re "\r?\n__VGPU_DRM_MISSING__" { exit 3 }
  timeout { exit 3 }
}

# virtio transport may be virtio-mmio, binding check should look at virtio_gpu driver directory
expect "# " {
  send "sh -lc 'ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1 && echo __VGPU_BIND_OK__ || echo __VGPU_BIND_BAD__'\r"
} timeout { exit 3 }
expect {
  -re "\r?\n__VGPU_BIND_OK__" {}
  -re "\r?\n__VGPU_BIND_BAD__" {
    send "ls -l /sys/bus/virtio/drivers/virtio_gpu/ 2>/dev/null || true\r"
    # emit literal $d via \u0024 to avoid Tcl variable substitution
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
# 2) Check run.sh exists at /root/run.sh
# 3) cd to /root and source run.sh to set PATH/LD_LIBRARY_PATH
# 4) Verify df_drivertest is in PATH
# 5) Run df_drivertest and check for DirectFB init messages
#
# NOTE: df_drivertest may segfault when killed due to a race condition in DirectFB2's
# fusion module (libfusion) during signal handling. When SIGTERM is sent, the signal
# handler starts cleanup while the "Fusion Dispatch" thread may still be accessing
# shared state, leading to a use-after-free crash. The test passes if DirectFB init
# messages appear, even if the program crashes afterward during cleanup.

# Step 0: Stop X11 to release DRM device (it holds /dev/dri/card0)
# Use pidof with fallback to ps/grep if pidof unavailable
expect "# " {
  send "sh -lc '\
    if command -v pidof >/dev/null 2>&1; then \
      pidof Xorg >/dev/null 2>&1 && kill \u0024(pidof Xorg) 2>/dev/null || true; \
    else \
      ps | grep Xorg | grep -v grep | awk \"{print \u00241}\" | xargs kill 2>/dev/null || true; \
    fi; \
    sleep 1; echo __X11_STOPPED__'\r"
}
expect "__X11_STOPPED__" {}

# Step 1: Check run.sh exists
expect "# " { send "test -f /root/run.sh && echo __RUNSH_OK__ || echo __DFB_RUNSH_MISSING__\r" }
expect {
  -re "\r?\n__RUNSH_OK__" {}
  -re "\r?\n__DFB_RUNSH_MISSING__" { exit 4 }
  timeout { exit 4 }
}

# Step 2: cd to /root and source run.sh
expect "# " { send "cd /root && . ./run.sh >/dev/null 2>&1; echo __SRC_DONE__\r" }
expect "__SRC_DONE__" {}

# Step 3: Verify df_drivertest is available
expect "# " { send "command -v df_drivertest && echo __APP_OK__ || echo __APP_MISS__\r" }
expect {
  -re "\r?\n__APP_OK__" {}
  -re "\r?\n__APP_MISS__" { exit 4 }
  timeout { exit 4 }
}

# Step 4: Run df_drivertest and check output (run in background, kill after delay)
expect "# " { send "df_drivertest >/tmp/dfb.log 2>&1 & sleep $env(DFB_SLEEP); kill \u0024! 2>/dev/null; head -30 /tmp/dfb.log\r" }
# Check for DRMKMS init message
expect "# " { send "grep -qi 'DRMKMS/System' /tmp/dfb.log && echo __DFB_OK__ || echo __DFB_FAIL__\r" }
expect {
  -re "\r?\n__DFB_OK__" {}
  -re "\r?\n__DFB_FAIL__" { exit 4 }
  timeout { exit 4 }
}
DONE

ret="$?"
set -e  # Re-enable errexit after capturing expect's return code

MESSAGES=(
  "PASS: headless vgpu + DirectFB2 checks"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-gpu basic checks failed (/dev/dri/card0 or virtio_gpu binding)"
  "FAIL: DirectFB2 check failed (run.sh/df_drivertest missing or no DRMKMS init messages)"
)

# Clean up pre-built ext4.img so other tests can use their own
if [ -f "ext4.img.bz2" ]; then
    rm -f ext4.img
fi

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
