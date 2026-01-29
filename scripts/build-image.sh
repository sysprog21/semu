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

function copy_buildroot_config
{
    local buildroot_config="configs/buildroot.config"
    local x11_config="configs/x11.config"
    local output_config="buildroot/.config"
    local merge_tool="buildroot/support/kconfig/merge_config.sh"

    if [ ! -f "$output_config" ]; then
        echo "Preparing initial Buildroot config..."

        # Check X11 option
        if [[ $BUILD_X11 -eq 1 ]]; then
            # Compile Buildroot with X11
            "$merge_tool" -m -r -O buildroot "$buildroot_config" "$x11_config"
        else
            # Compile Buildroot without X11
            cp -f "$buildroot_config" "$output_config"
        fi
    else
        echo "$output_config already exists, skipping copy"
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

    copy_buildroot_config
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

    if [[ $BUILD_EXTRA_PACKAGES -eq 1 ]]; then
        do_extra_packages
    fi

    if [[ $EXTERNAL_ROOT -eq 1 ]]; then
        echo "Copying rootfs.cpio to rootfs_full.cpio (external root mode)"
        cp -f buildroot/output/images/rootfs.cpio ./rootfs_full.cpio
        ASSERT ./scripts/rootfs_ext4.sh
    else
        echo "Copying rootfs.cpio to rootfs.cpio (initramfs mode)"
        cp -f buildroot/output/images/rootfs.cpio ./rootfs.cpio
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

function show_help {
    cat << EOF
Usage: $0 [--buildroot] [--linux] [--extra-packages] [--all] [--external-root] [--clean-build] [--help]

Options:
  --buildroot         Build Buildroot rootfs
  --x11               Build Buildroot with X11
  --extra-packages    Build extra packages along with Buildroot
  --linux             Build Linux kernel
  --all               Build both Buildroot and Linux kernel
  --external-root     Use external rootfs instead of initramfs
  --clean-build       Remove entire buildroot/ and/or linux/ directories before build
  --help              Show this message
EOF
    exit 1
}

BUILD_BUILDROOT=0
BUILD_X11=0
BUILD_EXTRA_PACKAGES=0
BUILD_LINUX=0
EXTERNAL_ROOT=0
CLEAN_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --buildroot)
            BUILD_BUILDROOT=1
            ;;
        --x11)
            BUILD_X11=1
            ;;
        --extra-packages)
            BUILD_EXTRA_PACKAGES=1
            ;;
        --linux)
            BUILD_LINUX=1
            ;;
        --all)
            BUILD_BUILDROOT=1
            BUILD_LINUX=1
            ;;
        --external-root)
            EXTERNAL_ROOT=1
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

function do_directfb
{
    export PATH=$PATH:$PWD/buildroot/output/host/bin
    export BUILDROOT_OUT=$PWD/buildroot/output/
    mkdir -p directfb

    # Build DirectFB2
    if [ ! -d DirectFB2 ]; then
        echo "Cloning DirectFB2..."
        ASSERT git clone https://github.com/directfb2/DirectFB2.git
    else
        echo "DirectFB2 already exists, skipping clone..."
    fi
    pushd DirectFB2
    cp ../configs/riscv-cross-file .
    ASSERT meson -Ddrmkms=true --cross-file riscv-cross-file build/riscv
    ASSERT meson compile -C build/riscv
    DESTDIR=$BUILDROOT_OUT/host/riscv32-buildroot-linux-gnu/sysroot meson install -C build/riscv
    DESTDIR=../../../directfb meson install -C build/riscv
    popd

    # Build DirectFB2 examples
    if [ ! -d DirectFB-examples ]; then
        echo "Cloning DirectFB-examples..."
        ASSERT git clone https://github.com/directfb2/DirectFB-examples.git
    else
        echo "DirectFB-examples already exists, skipping clone..."
    fi
    pushd DirectFB-examples/
    cp ../configs/riscv-cross-file .
    ASSERT meson --cross-file riscv-cross-file build/riscv
    ASSERT meson compile -C build/riscv
    DESTDIR=../../../directfb meson install -C build/riscv
    popd
}

function do_extra_packages
{
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-

    rm -rf extra_packages
    mkdir -p extra_packages
    mkdir -p extra_packages/root

    do_directfb && OK

    cp -r directfb/* extra_packages
    cp target/run.sh extra_packages/root/
}

if [[ $BUILD_BUILDROOT -eq 0 && $BUILD_LINUX -eq 0 ]]; then
    echo "Error: No build target specified. Use --buildroot, --linux, or --all."
    show_help
fi

if [[ $BUILD_EXTRA_PACKAGES -eq 1 && $BUILD_BUILDROOT -eq 0 ]]; then
    echo "Error: --extra-packages requires --buildroot to be specified."
    show_help
fi

if [[ $BUILD_X11 -eq 1 && $BUILD_BUILDROOT -eq 0 ]]; then
    echo "Error: --x11 requires --buildroot to be specified."
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

if [[ $BUILD_BUILDROOT -eq 1 ]]; then
    do_buildroot && OK
fi

if [[ $BUILD_LINUX -eq 1 ]]; then
    do_linux && OK
fi
