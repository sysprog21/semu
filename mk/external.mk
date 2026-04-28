# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#   _DATA_SHA1 : the checksum of the content in _DATA
#
# Artifacts live on the orphan `blob` branch of this repository. The
# `Publish prebuilt images` workflow (see .github/workflows/prebuilt.yml)
# will eventually republish them to a fixed-tag GitHub prerelease; once
# that release exists, switch COMMON_URL to
# https://github.com/sysprog21/semu/releases/download/prebuilt and update
# KERNEL_DATA_SHA1, INITRD_DATA_SHA1, and PREBUILT_INPUTS_SHA1 from the
# release body.

COMMON_URL = https://github.com/sysprog21/semu/raw/blob

# kernel
KERNEL_DATA_URL = $(COMMON_URL)/Image.bz2
KERNEL_DATA = Image
KERNEL_DATA_SHA1 = f33badc277a88c17ce36b28d229a3f99cbccef91

# initrd
INITRD_DATA_URL = $(COMMON_URL)/rootfs.cpio.bz2
INITRD_DATA = rootfs.cpio
INITRD_DATA_SHA1 = a63336a28e484ed9cd560652c336b93affe50126

define download
$($(T)_DATA):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --progress-bar -O -L -C - "$(strip $($(T)_DATA_URL))"
	$(Q)echo "$(strip $$($(T)_DATA_SHA1))  $$@.bz2" | $(SHA1SUM) -c -
	$(Q)bunzip2 $$@.bz2
endef

EXTERNAL_DATA = KERNEL INITRD
$(foreach T,$(EXTERNAL_DATA),$(eval $(download)))

# --- Stale-prebuilt detection -------------------------------------------
#
# The prebuilt Image and rootfs.cpio above are baked from a fixed set of
# input files (kernel/buildroot/busybox configs, the build script, and
# the init stub). When any of those change locally the prebuilt may no
# longer reflect the user's intent, so we compute the SHA1 of those
# inputs and compare against PREBUILT_INPUTS_SHA1 -- the value the
# `Publish prebuilt images` workflow recorded for the live release.
#
# Mismatch -> warn but do not auto-rebuild: a buildroot run takes the
# better part of an hour, so we let the user opt in via `make build-image`.
# Keep this list in sync with the INPUTS array in .ci/publish-prebuilt.sh
# and the `paths:` filter in .github/workflows/prebuilt.yml.
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
