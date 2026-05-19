#!/usr/bin/env bash
#
# Build an ext4 rootfs image from an existing cpio archive.
#
# Usage: rootfs_ext4.sh [SOURCE_CPIO] [OUT_IMG] [SIZE_MB] [EXTRA_DIR]
#
# Default values match the EXTROOT make path: read rootfs.cpio, produce
# ext4.img sized at 32 MiB. The 32 MiB default fits the buildroot userland
# with headroom; bump SIZE_MB for larger rootfs payloads. EXTRA_DIR, when
# given, is copied into the ext4 image after SOURCE_CPIO is extracted without
# changing SOURCE_CPIO itself.

set -euo pipefail

SRC_CPIO="${1:-rootfs.cpio}"
OUT_IMG="${2:-ext4.img}"
SIZE_MB="${3:-32}"
EXTRA_DIR="${4:-}"
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
OUT_DIR="$(cd "$(dirname "$OUT_IMG")" && pwd -P)"
OUT_ABS="$OUT_DIR/$(basename "$OUT_IMG")"
EXTRA_ABS=""
if [ -n "$EXTRA_DIR" ]; then
    if [ ! -d "$EXTRA_DIR" ]; then
        echo "[!] Extra directory not found: $EXTRA_DIR" >&2
        exit 1
    fi
    EXTRA_ABS="$(cd "$EXTRA_DIR" && pwd -P)"
fi
# `mktemp -d -t PREFIX` differs between GNU (PREFIX is a name) and BSD (PREFIX
# is a template) -- spell out the full template instead.
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/semu-rootfs.XXXXXX")"
OUT_TMP="$(mktemp "$OUT_DIR/.$(basename "$OUT_IMG").XXXXXX")"
trap 'rm -rf "$STAGE" "$OUT_TMP"' EXIT

echo "[*] Creating empty image: $OUT_IMG (${SIZE_MB} MiB)"
# bs=1024k works on both GNU and BSD dd; bs=1M is GNU-only and bs=1m is
# BSD-only.
dd if=/dev/zero of="$OUT_TMP" bs=1024k count="$SIZE_MB" >/dev/null 2>&1

echo "[*] Building ext4 filesystem"
echo "[*] Extracting $SRC_CPIO -> $STAGE"
if [ -n "$EXTRA_ABS" ]; then
    echo "[*] Applying extra files: $EXTRA_DIR"
fi
# -E lazy_*_init=0: do all init at mkfs time so the first guest mount does
#   not pay the lazy-init cost. Stripping the journal (-O ^has_journal)
#   would also speed mount, but the prebuilt Linux Image is built with
#   CONFIG_EXT4_USE_FOR_EXT2=n and refuses to mount a no-journal image.
fakeroot bash -c '
        set -e
        stage="$1"
        src_cpio="$2"
        extra_dir="$3"
        mkfs_ext4="$4"
        out_img="$5"

        cd "$stage"
        cpio -idm < "$src_cpio"
        if [ -n "$extra_dir" ]; then
            cp -a "$extra_dir"/. .
        fi
        chown -R 0:0 .
        "$mkfs_ext4" -q -F \
            -E lazy_itable_init=0,lazy_journal_init=0 \
            -d . "$out_img"
    ' sh "$STAGE" "$SRC_ABS" "$EXTRA_ABS" "$MKFS_EXT4" "$OUT_TMP"

mv -f "$OUT_TMP" "$OUT_ABS"
du -h "$OUT_ABS"
