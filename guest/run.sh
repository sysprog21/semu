#!/usr/bin/bash

# Run this script to install virtio-gpu module into
# the guest Linux system

mkdir -p /lib/modules/
cp -r ./lib/modules/* /lib/modules/

modprobe virtio-gpu
