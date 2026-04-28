#!/usr/bin/env bash
#
# Build an ext4 rootfs image from an existing cpio archive.
#
# Usage: rootfs_ext4.sh [SOURCE_CPIO] [OUT_IMG] [SIZE_MB]
#
# Default values match the EXTROOT make path: read rootfs.cpio, produce
# ext4.img sized at 32 MiB. The 32 MiB default fits the buildroot userland
# with headroom; bump SIZE_MB for larger rootfs payloads.

set -euo pipefail

SRC_CPIO="${1:-rootfs.cpio}"
OUT_IMG="${2:-ext4.img}"
SIZE_MB="${3:-32}"
MKFS_EXT4="${MKFS_EXT4:-mkfs.ext4}"

if [ ! -f "$SRC_CPIO" ]; then
    echo "[!] Source cpio not found: $SRC_CPIO" >&2
    exit 1
fi

if ! command -v fakeroot >/dev/null 2>&1; then
    echo "[!] fakeroot is required to build the ext4 image" >&2
    exit 1
fi

if ! command -v "$MKFS_EXT4" >/dev/null 2>&1; then
    echo "[!] mkfs.ext4 is required to build the ext4 image" >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "$SRC_CPIO")" && pwd -P)"
SRC_ABS="$SRC_DIR/$(basename "$SRC_CPIO")"
# `mktemp -d -t PREFIX` differs between GNU (PREFIX is a name) and BSD (PREFIX
# is a template) -- spell out the full template instead.
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/semu-rootfs.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

echo "[*] Extracting $SRC_CPIO -> $STAGE"
( cd "$STAGE" && fakeroot bash -c "cpio -idm < '$SRC_ABS'" )

echo "[*] Creating empty image: $OUT_IMG (${SIZE_MB} MiB)"
# bs=1024k works on both GNU and BSD dd; bs=1M is GNU-only and bs=1m is
# BSD-only.
dd if=/dev/zero of="$OUT_IMG" bs=1024k count="$SIZE_MB" >/dev/null 2>&1

echo "[*] Building ext4 filesystem"
# -E lazy_*_init=0: do all init at mkfs time so the first guest mount does
#   not pay the lazy-init cost. Stripping the journal (-O ^has_journal)
#   would also speed mount, but the prebuilt Linux Image is built with
#   CONFIG_EXT4_USE_FOR_EXT2=n and refuses to mount a no-journal image.
fakeroot "$MKFS_EXT4" -q -F \
    -E lazy_itable_init=0,lazy_journal_init=0 \
    -d "$STAGE" "$OUT_IMG"

du -h "$OUT_IMG"
