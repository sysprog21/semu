#!/usr/bin/env bash
#
# Compress the prebuilt Image and rootfs.cpio in cwd, write a sha1
# manifest, hash the input files that define the prebuilt's contents,
# and print all three sums in KEY=VAL form on stdout so callers can
# splice them into release notes, GITHUB_OUTPUT, or whatever else.
#
# Inputs (in cwd):
#   Image
#   rootfs.cpio
#   plus the source inputs listed in INPUTS below (config + scripts +
#   target/init that define the buildroot/kernel content)
#
# Outputs (in cwd):
#   Image.bz2
#   rootfs.cpio.bz2
#   prebuilt.sha1   -- two-line manifest in standard `sha1sum` format
#
# Stdout (machine-readable, one assignment per line):
#   kernel_sha1=<sha1 of Image.bz2>
#   initrd_sha1=<sha1 of rootfs.cpio.bz2>
#   inputs_sha1=<sha1 of the concatenated input files>

set -euo pipefail

# Pick a SHA1 tool. macOS dropped `sha1sum` from the base system; the
# coreutils-style `shasum -a 1` is the portable fallback.
if command -v sha1sum >/dev/null 2>&1; then
    SHA1=(sha1sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA1=(shasum -a 1)
else
    echo "[!] Need sha1sum (Linux) or shasum (macOS) on PATH" >&2
    exit 1
fi

# Keep this list in sync with PREBUILT_INPUTS in mk/external.mk and the
# `paths:` filter in .github/workflows/prebuilt.yml.
INPUTS=(
    configs/linux.config
    configs/busybox.config
    configs/buildroot.config
    scripts/build-image.sh
    scripts/rootfs_ext4.sh
    target/init
)

for f in Image rootfs.cpio "${INPUTS[@]}"; do
    if [ ! -f "$f" ]; then
        echo "[!] Missing $f -- run scripts/build-image.sh --all first" >&2
        exit 1
    fi
done

bzip2 -k -f Image
bzip2 -k -f rootfs.cpio

KERNEL_SHA1=$("${SHA1[@]}" Image.bz2       | awk '{print $1}')
INITRD_SHA1=$("${SHA1[@]}" rootfs.cpio.bz2 | awk '{print $1}')
# Concatenate inputs in deterministic order and hash the stream. Matches
# the make-time computation in mk/external.mk so they compare directly.
INPUTS_SHA1=$(cat "${INPUTS[@]}" | "${SHA1[@]}" | awk '{print $1}')

# Write the human-friendly checksum manifest. Format matches `sha1sum -c`
# so the file works as input to that tool unchanged.
{
    echo "$KERNEL_SHA1  Image.bz2"
    echo "$INITRD_SHA1  rootfs.cpio.bz2"
} > prebuilt.sha1

# Echo the manifest + inputs hash to stderr for visibility in CI logs
# without polluting the parseable stdout block below.
{
    cat prebuilt.sha1
    echo "inputs_sha1: $INPUTS_SHA1"
} >&2

echo "kernel_sha1=$KERNEL_SHA1"
echo "initrd_sha1=$INITRD_SHA1"
echo "inputs_sha1=$INPUTS_SHA1"
