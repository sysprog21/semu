#!/usr/bin/bash

IMG=ext4.img
KERNEL_VER=6.1.77
SRC=linux/out/lib/modules/$KERNEL_VER
DEST=rootfs/lib/modules/$KERNEL_VER

FILES='kernel/drivers/gpu/drm/drm.ko
       kernel/drivers/gpu/drm/drm_kms_helper.ko
       kernel/drivers/gpu/drm/drm_panel_orientation_quirks.ko
       kernel/drivers/gpu/drm/drm_shmem_helper.ko
       kernel/drivers/gpu/drm/virtio/virtio-gpu.ko
       kernel/drivers/i2c/algos/i2c-algo-bit.ko
       kernel/drivers/i2c/i2c-core.ko
       kernel/drivers/virtio/virtio_dma_buf.ko
       modules.dep'

mkdir -p $DEST/kernel/drivers/gpu/drm/
mkdir -p $DEST/kernel/drivers/gpu/drm/virtio/
mkdir -p $DEST/kernel/drivers/i2c/algos/
mkdir -p $DEST/kernel/drivers/virtio/

for file in $FILES; do
    cp -f $SRC/$file $DEST/$file
done

cp guest/run.sh rootfs

# kernel objects of virtio-gpu and root files requires ~35MiB of space
dd if=/dev/zero of=${IMG} bs=4k count=9000
mkfs.ext4 -F ${IMG} -d rootfs

rm -rf rootfs

# show image size
du -h ${IMG}
