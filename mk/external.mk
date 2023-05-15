# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#   _DATA_SHA1 : the checksum of the content in _DATA

# kernel
KERNEL_DATA_URL = https://github.com/jserv/semu/raw/blob/Image
KERNEL_DATA = Image
KERNEL_DATA_SHA1 = 4a1cf1465c1df564dbd88d9cb6f46773d2555ceb

define download
$($(T)_DATA):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --progress-bar -O -L -C - "$(strip $($(T)_DATA_URL))"
	$(Q)echo "$(strip $$($(T)_DATA_SHA1))  $$@" | $(SHA1SUM) -c
endef

EXTERNAL_DATA = KERNEL
$(foreach T,$(EXTERNAL_DATA),$(eval $(download))) 
