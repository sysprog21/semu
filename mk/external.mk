# For each external target, the following must be defined in advance:
#   _DATA_URL : the hyperlink which points to archive.
#   _DATA : the file to be read by specific executable.
#
# Artifacts are published as assets on the fixed-tag prebuilt GitHub
# prerelease by .github/workflows/prebuilt.yml. The expected SHA-1 of
# each archive is read from the prebuilt.sha1 manifest published
# alongside the archives, so checksum updates require no edit here.

COMMON_URL = https://github.com/sysprog21/semu/releases/download/prebuilt

PREBUILT_MANIFEST = prebuilt.sha1
PREBUILT_MANIFEST_URL = $(COMMON_URL)/$(PREBUILT_MANIFEST)

# kernel
KERNEL_DATA_URL = $(COMMON_URL)/Image.bz2
KERNEL_DATA = Image

# initrd
INITRD_DATA_URL = $(COMMON_URL)/rootfs.cpio.bz2
INITRD_DATA = rootfs.cpio

$(PREBUILT_MANIFEST): FORCE
	$(VECHO) "  GET\t$@\n"
	$(Q)if curl --fail --retry 3 --retry-delay 1 --progress-bar \
	    -L -o "$@.part" "$(PREBUILT_MANIFEST_URL)"; then \
	    if [ -f "$@" ] && cmp -s "$@" "$@.part"; then \
	        rm -f "$@.part"; \
	    else \
	        mv "$@.part" "$@"; \
	    fi; \
	else \
	    rm -f "$@.part"; \
	    if [ -f "$@" ]; then \
	        $(PRINTF) "  KEEP\t$@ (offline; using cached manifest)\n"; \
	    else \
	        echo "manifest fetch failed and no cached manifest; cannot proceed" >&2; \
	        exit 1; \
	    fi; \
	fi

define download
# Download to a .part file so an interrupted curl never lands a
# corrupt or incomplete .bz2 that a later run mistakes for valid input.
# Curl resume (-C -) is intentionally NOT used: a fully-downloaded .part
# left over from a previous run, e.g. interrupted before sha1 verify,
# would make curl request a byte range past EOF, the server replies
# HTTP 416, and curl exits non-zero, a permanent self-inflicted
# deadlock. These files are 5 to 7 MiB; a fresh GET is cheap.
#
# Look up the expected SHA-1 by archive basename in the release
# manifest, then verify the .part against it. Decompress to a .tmp
# file and rename only on success, so an interrupted bunzip2 cannot
# leave a half-decompressed Image or rootfs.cpio that make would treat
# as a valid up-to-date target on the next invocation.
$($(T)_DATA): $(PREBUILT_MANIFEST) | prebuilt-check
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --fail --retry 3 --retry-delay 1 --progress-bar \
	    -L -o "$$@.bz2.part" "$(strip $($(T)_DATA_URL))" \
	    || { rm -f "$$@.bz2.part"; exit 1; }
	$(Q)expected=$$$$(awk -v f="$(notdir $($(T)_DATA_URL))" '$$$$2==f{print $$$$1}' $(PREBUILT_MANIFEST)); \
	    [ -n "$$$$expected" ] || { echo "verify: no $(notdir $($(T)_DATA_URL)) entry in $(PREBUILT_MANIFEST)" >&2; rm -f "$$@.bz2.part"; exit 1; }; \
	    echo "$$$$expected  $$@.bz2.part" | $(SHA1SUM) -c - \
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
# inputs and compare against the publisher's recorded inputs hash --
# the third line of prebuilt.sha1, written by .ci/publish-prebuilt.sh
# under the virtual name 'inputs'.
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

# Read the publisher's inputs hash from the downloaded manifest at
# recipe time, after the manifest refresh above has had a chance to run.
.PHONY: prebuilt-check
prebuilt-check: $(PREBUILT_MANIFEST)
	$(Q)manifest_sha1=$$(awk '$$2 == "inputs" {print $$1}' $(PREBUILT_MANIFEST)); \
	    if [ -n "$$manifest_sha1" ]; then \
	        expected=$(words $(PREBUILT_INPUTS)); \
	        found=0; \
	        for f in $(PREBUILT_INPUTS); do [ -f "$$f" ] && found=$$((found + 1)); done; \
	        if [ "$$found" -eq "$$expected" ]; then \
	            live_sha1=$$(cat $(PREBUILT_INPUTS) | $(SHA1SUM) | awk '{print $$1}'); \
	            if [ "$$live_sha1" != "$$manifest_sha1" ]; then \
	                echo "warning: Local kernel/rootfs inputs ($$live_sha1) differ from" >&2; \
	                echo "warning: the prebuilt's recorded inputs ($$manifest_sha1)." >&2; \
	                echo "warning: The downloaded Image/rootfs.cpio do not reflect your local" >&2; \
	                echo "warning: configs. Run \`make build-image\` to rebuild from source." >&2; \
	            fi; \
	        fi; \
	    fi
