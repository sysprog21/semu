#!/usr/bin/bash

ROOTFS_CPIO="rootfs_full.cpio"
IMG="ext4.img"
IMG_SIZE=$((1024 * 1024 * 1024)) # 1GB
IMG_SIZE_BLOCKS=$((${IMG_SIZE} / 4096)) # IMG_SIZE / 4k

DIR=rootfs

echo "[*] Remove old rootfs directory..."
rm -rf $DIR
mkdir -p $DIR

cp -r extra_packages/* $DIR 

echo "[*] Extract CPIO"
pushd $DIR
cpio -idmv < ../$ROOTFS_CPIO 
popd

echo "[*] Create empty image"
dd if=/dev/zero of=${IMG} bs=4k count=${IMG_SIZE_BLOCKS}

echo "[*] Create ext4 rootfs image"
fakeroot mkfs.ext4 -F ${IMG} -d $DIR

# Show image size
du -h ${IMG}
