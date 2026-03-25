#!/usr/bin/env bash

function ASSERT
{
    $*
    RES=$?
    if [ $RES -ne 0 ]; then
        echo 'Assert failed: "' $* '"'
        exit $RES
    fi
}

PASS_COLOR='\e[32;01m'
NO_COLOR='\e[0m'
function OK
{
    printf " [ ${PASS_COLOR} OK ${NO_COLOR} ]\n"
}

PARALLEL="-j$(nproc)"

DIRECTFB2_REPO="https://github.com/directfb2/DirectFB2"
DIRECTFB2_REV="7d4682d0cc092ed2f28c903175d1a0c104e9e9a8"
DIRECTFB_EXAMPLES_REPO="https://github.com/directfb2/DirectFB-examples"
DIRECTFB_EXAMPLES_REV="eecf1019b29933a45578e62aea5f08a884d30fbc"
TEST_TOOLS_SIZE_MB=192

function safe_copy {
    local src="$1"
    local dst="$2"

    if [ ! -f "$dst" ]; then
        echo "Copying $src -> $dst"
        cp -f "$src" "$dst"
    else
        echo "$dst already exists, skipping copy"
    fi
}

function checkout_repo_rev
{
    local dir="$1"
    local repo="$2"
    local rev="$3"

    if [ ! -d "$dir/.git" ]; then
        echo "Cloning $dir..."
        ASSERT git clone "$repo" "$dir"
    else
        echo "$dir already exists, reusing clone..."
    fi

    pushd "$dir"
    if ! git cat-file -e "$rev^{commit}" 2>/dev/null; then
        ASSERT git fetch origin
    fi
    ASSERT git checkout --detach "$rev"
    popd
}

function meson_setup_or_reconfigure
{
    local build_dir="$1"
    shift

    if [ -f "$build_dir/build.ninja" ]; then
        if ! meson setup --reconfigure "$@" "$build_dir"; then
            echo "Recreating stale Meson build directory: $build_dir"
            rm -rf "$build_dir"
            ASSERT meson setup "$@" "$build_dir"
        fi
    else
        ASSERT meson setup "$@" "$build_dir"
    fi
}

function do_buildroot
{
    if [ ! -d buildroot ]; then
        echo "Cloning Buildroot..."
        ASSERT git clone https://github.com/buildroot/buildroot -b 2025.02.x --depth=1
    else
        echo "buildroot/ already exists, skipping clone"
    fi

    safe_copy configs/buildroot.config buildroot/.config
    safe_copy configs/busybox.config buildroot/busybox.config
    cp -f target/init buildroot/fs/cpio/init

    # Otherwise, the error below raises:
    #   You seem to have the current working directory in your
    #   LD_LIBRARY_PATH environment variable. This doesn't work.
    unset LD_LIBRARY_PATH
    pushd buildroot
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    popd

    # Always publish the cpio. It is the canonical buildroot output and
    # serves both as the source for the ext4 image and as the legacy
    # initramfs payload (when ENABLE_EXTERNAL_ROOT=0).
    echo "Publishing rootfs.cpio"
    cp -f buildroot/output/images/rootfs.cpio ./rootfs.cpio

    # Build ext4.img unless --no-ext4 was passed. The make default
    # (ENABLE_EXTERNAL_ROOT=1) boots from /dev/vda and needs this image.
    # --no-ext4 is the escape hatch for users who only want the legacy
    # initramfs path or do not have fakeroot/mkfs.ext4 installed.
    if [[ $NO_EXT4 -eq 1 ]]; then
        echo "Skipping ext4.img build (--no-ext4)"
    else
        ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./ext4.img
        if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
            do_extra_packages
            ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./test-tools.img \
                "$TEST_TOOLS_SIZE_MB" ./extra_packages
        fi
    fi
}

function do_linux
{
    if [ ! -d linux ]; then
        echo "Cloning Linux kernel..."
        ASSERT git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git -b linux-6.12.y --depth=1
    else
        echo "linux/ already exists, skipping clone"
    fi

    safe_copy configs/linux.config linux/.config

    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd linux
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    cp -f arch/riscv/boot/Image ../Image
    popd
}

function do_directfb
{
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export BUILDROOT_OUT=$PWD/buildroot/output/
    export DIRECTFB_STAGE=$PWD/directfb
    mkdir -p directfb

    # Build DirectFB2
    checkout_repo_rev DirectFB2 "$DIRECTFB2_REPO" "$DIRECTFB2_REV"
    pushd DirectFB2
    cp ../configs/riscv-cross-file .
    meson_setup_or_reconfigure build/riscv -Ddrmkms=true --cross-file \
        riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR=$BUILDROOT_OUT/host/riscv32-buildroot-linux-gnu/sysroot meson install -C build/riscv
    ASSERT env DESTDIR=$DIRECTFB_STAGE meson install -C build/riscv
    popd

    # Build DirectFB2 examples
    checkout_repo_rev DirectFB-examples "$DIRECTFB_EXAMPLES_REPO" \
        "$DIRECTFB_EXAMPLES_REV"
    pushd DirectFB-examples/
    cp ../configs/riscv-cross-file .
    meson_setup_or_reconfigure build/riscv --cross-file riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR=$DIRECTFB_STAGE meson install -C build/riscv
    popd
}

function do_extra_packages
{
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-

    rm -rf directfb extra_packages
    mkdir -p directfb
    mkdir -p extra_packages
    mkdir -p extra_packages/root

    do_directfb && OK

    if ! find directfb -mindepth 1 -print -quit | grep -q .; then
        echo "Error: DirectFB staging tree is empty."
        exit 1
    fi

    ASSERT cp -r directfb/. extra_packages/
    ASSERT cp target/local-env.sh extra_packages/root/
}

function show_help {
    cat << EOF
Usage: $0 [--buildroot] [--linux] [--directfb2-test] [--all] [--no-ext4] [--clean-build] [--help]

Options:
  --buildroot         Build Buildroot userland (produces rootfs.cpio and,
                      unless --no-ext4 is given, ext4.img for vda boot)
  --directfb2-test    Build test-tools.img with the DirectFB2 test payload
  --linux             Build the Linux kernel
  --all               Build both Buildroot and Linux
  --no-ext4           Skip ext4.img generation; produce only rootfs.cpio
                      (matches the legacy ENABLE_EXTERNAL_ROOT=0 path)
  --clean-build       Remove buildroot/ and/or linux/ before building;
                      with --directfb2-test, also remove DirectFB2 sources
  --help              Show this message
EOF
    exit 1
}

BUILD_BUILDROOT=0
BUILD_DIRECTFB_TEST=0
BUILD_LINUX=0
NO_EXT4=0
CLEAN_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --buildroot)
            BUILD_BUILDROOT=1
            ;;
        --directfb2-test)
            BUILD_BUILDROOT=1
            BUILD_DIRECTFB_TEST=1
            ;;
        --linux)
            BUILD_LINUX=1
            ;;
        --all)
            BUILD_BUILDROOT=1
            BUILD_LINUX=1
            ;;
        --no-ext4)
            NO_EXT4=1
            ;;
        --clean-build)
            CLEAN_BUILD=1
            ;;
        --help|-h)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
    shift
done

if [[ $BUILD_BUILDROOT -eq 0 && $BUILD_LINUX -eq 0 ]]; then
    echo "Error: No build target specified. Use --buildroot, --linux, or --all."
    show_help
fi

if [[ $BUILD_DIRECTFB_TEST -eq 1 && $NO_EXT4 -eq 1 ]]; then
    echo "Error: --directfb2-test requires an ext4 image; remove --no-ext4."
    show_help
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_BUILDROOT -eq 1 && -d buildroot ]]; then
    echo "Removing buildroot/ for clean build..."
    rm -rf buildroot
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_LINUX -eq 1 && -d linux ]]; then
    echo "Removing linux/ for clean build..."
    rm -rf linux
fi

if [[ $CLEAN_BUILD -eq 1 && $BUILD_DIRECTFB_TEST -eq 1 ]]; then
    echo "Removing DirectFB2 sources for clean build..."
    rm -rf DirectFB2 DirectFB-examples directfb extra_packages
fi

if [[ $BUILD_BUILDROOT -eq 1 ]]; then
    do_buildroot && OK
fi

if [[ $BUILD_LINUX -eq 1 ]]; then
    do_linux && OK
fi
