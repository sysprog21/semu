# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#   _DATA_SHA1 : the checksum of the content in _DATA

COMMON_URL = https://github.com/sysprog21/semu/raw/blob

# kernel
KERNEL_DATA_URL = https://github.com/Mes0903/semu/raw/blob/Image.bz2
KERNEL_DATA = Image
KERNEL_DATA_SHA1 = 0666dbb915cdb2275c73183caa45524b57e5ea7d

# initrd
INITRD_DATA_URL = $(COMMON_URL)/rootfs.cpio.bz2
INITRD_DATA = rootfs.cpio
INITRD_DATA_SHA1 = a63336a28e484ed9cd560652c336b93affe50126

# guest disk
GUEST_DISK_DATA_URL = https://github.com/Mes0903/semu/raw/blob/ext4.img.bz2
GUEST_DISK_DATA = ext4.img
GUEST_DISK_DATA_SHA1 = 83ed49c16d341bdf3210141d5f6d5842b77a6adc

define download
$($(T)_DATA):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --progress-bar -O -L -C - "$(strip $($(T)_DATA_URL))"
	$(Q)echo "$(strip $$($(T)_DATA_SHA1))  $$@.bz2" | $(SHA1SUM) -c -
	$(Q)bunzip2 $$@.bz2
endef

EXTERNAL_DATA = KERNEL INITRD GUEST_DISK
$(foreach T,$(EXTERNAL_DATA),$(eval $(download)))
