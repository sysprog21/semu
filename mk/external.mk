# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#   _DATA_SHA1 : the checksum of the content in _DATA
#
# Artifacts are published as assets on the fixed-tag prebuilt GitHub
# prerelease by .github/workflows/prebuilt.yml. Update the SHA1 values
# below from the release body whenever the workflow republishes a new
# build.

COMMON_URL = https://github.com/sysprog21/semu/releases/download/prebuilt

# kernel
KERNEL_DATA_URL = $(COMMON_URL)/Image.bz2
KERNEL_DATA = Image
KERNEL_DATA_SHA1 = 39d273097f21a1bf38fd93b96a3d7459f843bc84

# initrd
INITRD_DATA_URL = $(COMMON_URL)/rootfs.cpio.bz2
INITRD_DATA = rootfs.cpio
INITRD_DATA_SHA1 = 9df154cdf58103e953ccdf0d40736cadf9318b12

define download
# Download to a .part file so an interrupted curl never lands a
# corrupt or incomplete .bz2 that a later run mistakes for valid input.
# Curl resume (-C -) is intentionally NOT used: a fully-downloaded .part
# left over from a previous run, e.g. interrupted before sha1 verify,
# would make curl request a byte range past EOF, the server replies
# HTTP 416, and curl exits non-zero, a permanent self-inflicted
# deadlock. These files are 5 to 7 MiB; a fresh GET is cheap.
#
# Decompress to a .tmp file and rename only on success, so an
# interrupted bunzip2 cannot leave a half-decompressed Image or
# rootfs.cpio that make would treat as a valid up-to-date target on the
# next invocation.
$($(T)_DATA):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --fail --retry 3 --retry-delay 1 --progress-bar \
	    -L -o "$$@.bz2.part" "$(strip $($(T)_DATA_URL))" \
	    || { rm -f "$$@.bz2.part"; exit 1; }
	$(Q)echo "$(strip $$($(T)_DATA_SHA1))  $$@.bz2.part" | $(SHA1SUM) -c - \
	    || { rm -f "$$@.bz2.part"; exit 1; }
	$(Q)mv "$$@.bz2.part" "$$@.bz2"
	$(Q)bunzip2 -c "$$@.bz2" > "$$@.tmp" \
	    || { rm -f "$$@.tmp"; exit 1; }
	$(Q)mv "$$@.tmp" "$$@"
	$(Q)rm -f "$$@.bz2"
endef

EXTERNAL_DATA = KERNEL INITRD
$(foreach T,$(EXTERNAL_DATA),$(eval $(download)))

# --- Stale-prebuilt detection -------------------------------------------
#
# The prebuilt Image and rootfs.cpio above are baked from a fixed set of
# input files (kernel/buildroot/busybox configs, the build script, and
# the init stub). When any of those change locally the prebuilt may no
# longer reflect the user's intent, so we compute the SHA1 of those
# inputs and compare against PREBUILT_INPUTS_SHA1, the value the
# Publish prebuilt images workflow recorded for the live release.
#
# Mismatch -> warn but do not auto-rebuild: a buildroot run takes the
# better part of an hour, so we let the user opt in via make build-image.
# Keep this list in sync with the INPUTS array in .ci/publish-prebuilt.sh
# and the paths filter in .github/workflows/prebuilt.yml.
PREBUILT_INPUTS := \
    configs/linux.config \
    configs/busybox.config \
    configs/buildroot.config \
    scripts/build-image.sh \
    scripts/rootfs_ext4.sh \
    target/init

PREBUILT_INPUTS_SHA1 = 1ae09da49a6d7ce44e10d04a682950b295b3b77c

# Compute the live hash only when *every* input file exists. A partial
# tree would otherwise silently hash the present subset and trip a bogus
# "stale" warning instead of the more useful "your tree is incomplete"
# signal. The shell-side count compare is portable across BSD/GNU.
LIVE_INPUTS_SHA1 := $(shell \
    expected=$(words $(PREBUILT_INPUTS)); \
    found=0; \
    for f in $(PREBUILT_INPUTS); do [ -f "$$f" ] && found=$$((found + 1)); done; \
    if [ "$$found" -eq "$$expected" ]; then \
        cat $(PREBUILT_INPUTS) | $(SHA1SUM) | awk '{print $$1}'; \
    fi)

# Skip the comparison until PREBUILT_INPUTS_SHA1 is real (the all-zero
# placeholder is the bootstrap state before the first prebuilt run).
ifneq ($(PREBUILT_INPUTS_SHA1),0000000000000000000000000000000000000000)
ifneq ($(LIVE_INPUTS_SHA1),)
ifneq ($(LIVE_INPUTS_SHA1),$(PREBUILT_INPUTS_SHA1))
$(warning Local kernel/rootfs inputs ($(LIVE_INPUTS_SHA1)) differ from)
$(warning the prebuilt's recorded inputs ($(PREBUILT_INPUTS_SHA1)).)
$(warning The downloaded Image/rootfs.cpio do not reflect your local)
$(warning configs. Run `make build-image` to rebuild from source.)
endif
endif
endif
