#!/usr/bin/env sh
set -eu

TARGET_DIR="${1:?TARGET_DIR is required}"
INITTAB="${TARGET_DIR}/etc/inittab"
HVC0_LINE='hvc0::respawn:/sbin/getty -L  hvc0 0 vt100 # SEMU_VIRTIO_CONSOLE'

if [ ! -f "${INITTAB}" ]; then
    exit 0
fi

if ! grep -q 'SEMU_VIRTIO_CONSOLE' "${INITTAB}"; then
    printf '\n%s\n' "${HVC0_LINE}" >> "${INITTAB}"
fi
