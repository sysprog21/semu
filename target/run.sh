#!/usr/bin/bash

# Install kernel modules
if [ -d "/lib/modules/" ]; then
  mkdir -p /lib/modules/
  cp -r ./lib/modules/* /lib/modules/
fi

# Install DirectFB and examples
cp -r ./usr/local /usr/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib
export PATH=$PATH:/usr/local/bin/

# Load kernel modules if exist
# modprobe virtio-gpu
