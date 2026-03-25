#!/usr/bin/env bash
#
# Compress the prebuilt Image, rootfs.cpio, and test-tools.img in cwd, write a
# sha1 manifest, hash the input files that define the prebuilt's contents, and
# print all four sums in KEY=VAL form on stdout so callers can splice them into
# release notes, GITHUB_OUTPUT, or whatever else.
#
# Inputs (in cwd):
#   Image
#   rootfs.cpio
#   test-tools.img
#   plus the source inputs listed in INPUTS below (config + scripts +
#   target files that define the prebuilt content)
#
# Outputs (in cwd):
#   Image.bz2
#   rootfs.cpio.bz2
#   test-tools.img.bz2
#   prebuilt.sha1   -- four-line manifest in sha1sum format. The
#                      first three lines verify the published archives;
#                      the fourth uses the virtual name 'inputs' to
#                      publish the SHA-1 of the concatenated input
#                      files so drift-detection consumers can read it
#                      directly from the release.
#
# Stdout (machine-readable, one assignment per line):
#   kernel_sha1=<sha1 of Image.bz2>
#   initrd_sha1=<sha1 of rootfs.cpio.bz2>
#   test_tools_sha1=<sha1 of test-tools.img.bz2>
#   inputs_sha1=<sha1 of the concatenated input files>

set -euo pipefail

# Pick a SHA1 tool. macOS dropped sha1sum from the base system; the
# coreutils-style 'shasum -a 1' is the portable fallback.
if command -v sha1sum >/dev/null 2>&1; then
    SHA1=(sha1sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA1=(shasum -a 1)
else
    echo "[!] Need sha1sum (Linux) or shasum (macOS) on PATH" >&2
    exit 1
fi

# Keep this list in sync with PREBUILT_INPUTS in mk/external.mk and the
# 'paths:' filter in .github/workflows/prebuilt.yml.
INPUTS=(
    configs/linux.config
    configs/busybox.config
    configs/buildroot.config
    configs/riscv-cross-file
    scripts/build-image.sh
    scripts/rootfs_ext4.sh
    target/init
    target/local-env.sh
)

for f in Image rootfs.cpio test-tools.img "${INPUTS[@]}"; do
    if [ ! -f "$f" ]; then
        echo "[!] Missing $f -- run scripts/build-image.sh --all --directfb2-test first" >&2
        exit 1
    fi
done

bzip2 -k -f Image
bzip2 -k -f rootfs.cpio
bzip2 -k -f test-tools.img

KERNEL_SHA1=$("${SHA1[@]}" Image.bz2       | awk '{print $1}')
INITRD_SHA1=$("${SHA1[@]}" rootfs.cpio.bz2 | awk '{print $1}')
TEST_TOOLS_SHA1=$("${SHA1[@]}" test-tools.img.bz2 | awk '{print $1}')
# Concatenate inputs in deterministic order and hash the stream. Matches
# the make-time computation in mk/external.mk so they compare directly.
INPUTS_SHA1=$(cat "${INPUTS[@]}" | "${SHA1[@]}" | awk '{print $1}')

# Write the manifest. The first three lines match 'sha1sum -c' format for
# the real archives; the fourth line uses the virtual filename 'inputs'
# to publish the input-fingerprint hash so consumers (mk/external.mk's
# drift warning, .github/workflows/main.yml's PR drift detection) can
# read it from the release without parsing the release-body markdown.
{
    echo "$KERNEL_SHA1  Image.bz2"
    echo "$INITRD_SHA1  rootfs.cpio.bz2"
    echo "$TEST_TOOLS_SHA1  test-tools.img.bz2"
    echo "$INPUTS_SHA1  inputs"
} > prebuilt.sha1

# Echo the manifest + inputs hash to stderr for visibility in CI logs
# without polluting the parseable stdout block below.
{
    cat prebuilt.sha1
    echo "inputs_sha1: $INPUTS_SHA1"
} >&2

echo "kernel_sha1=$KERNEL_SHA1"
echo "initrd_sha1=$INITRD_SHA1"
echo "test_tools_sha1=$TEST_TOOLS_SHA1"
echo "inputs_sha1=$INPUTS_SHA1"
