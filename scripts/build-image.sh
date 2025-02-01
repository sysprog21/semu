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

function do_buildroot
{
    ASSERT git clone https://github.com/buildroot/buildroot -b 2024.11.1 --depth=1
    cp -f configs/buildroot.config buildroot/.config
    cp -f configs/busybox.config buildroot/busybox.config
    # Otherwise, the error below raises:
    #   You seem to have the current working directory in your
    #   LD_LIBRARY_PATH environment variable. This doesn't work.
    unset LD_LIBRARY_PATH
    pushd buildroot
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    popd
    cp -f buildroot/output/images/rootfs.cpio ./
}

function do_linux
{
    ASSERT git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git -b linux-6.1.y --depth=1
    cp -f configs/linux.config linux/.config
    export PATH="$PWD/buildroot/output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd linux
    ASSERT make olddefconfig
    ASSERT make $PARALLEL
    cp -f arch/riscv/boot/Image ../Image
    popd
}

do_buildroot && OK
do_linux && OK
