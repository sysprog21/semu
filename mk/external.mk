# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#   _DATA_SHA1 : the checksum of the content in _DATA

BASE_URL = https://github.com/d0iasm/rvemu/raw/main/bin/xv6

# kernel
KERNEL_DATA_URL = $(BASE_URL)/kernel.bin
KERNEL_DATA = kernel.bin
KERNEL_DATA_SHA1 = 7dd1fe7f470389ac991d1615aaf8ac7fc6731330

# rootfs
ROOTFS_DATA_URL = $(BASE_URL)/fs.img
ROOTFS_DATA = fs.img
ROOTFS_DATA_SHA1 = 27fd366b4383f1176f44095fa270d23e9b3f4deb

define download
$($(T)_DATA):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --progress-bar -O -L -C - "$(strip $($(T)_DATA_URL))"
	$(Q)echo "$(strip $$($(T)_DATA_SHA1))  $$@" | $(SHA1SUM) -c
endef

EXTERNAL_DATA = KERNEL ROOTFS
$(foreach T,$(EXTERNAL_DATA),$(eval $(download))) 
