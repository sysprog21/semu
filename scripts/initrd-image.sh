#!/usr/bin/bash

IMG=ext4.img
KERNEL_VER=$(git -C linux/ tag | sed "s/^v//")
SRC=linux/out/lib/modules/$KERNEL_VER
DEST=rootfs/lib/modules/$KERNEL_VER

# Add path of kernel modules to load with dependency file, for example:
# FILES='kernel/drivers/gpu/drm/drm.ko
#        modules.dep'
FILES=''

for file in $FILES; do
    mkdir -p `dirname $DEST/$file`
    cp -f $SRC/$file $DEST/$file
done

cp -r directfb rootfs
cp target/run.sh rootfs

#  DirectFB-example requires ~60MiB of space
dd if=/dev/zero of=${IMG} bs=4k count=20000
mkfs.ext4 -F ${IMG} -d rootfs

rm -rf rootfs

# show image size
du -h ${IMG}
