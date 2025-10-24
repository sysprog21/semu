include mk/common.mk
include mk/check-libs.mk

CC ?= gcc
CFLAGS := -O2 -g -Wall -Wextra
CFLAGS += -include common.h

# clock frequency
CLOCK_FREQ ?= 65000000
DT_CFLAGS := -D CLOCK_FREQ=$(CLOCK_FREQ)
CFLAGS += $(DT_CFLAGS)

OBJS_EXTRA :=
# command line option
OPTS :=

LDFLAGS :=

# virtio-blk
ENABLE_VIRTIOBLK ?= 1
$(call set-feature, VIRTIOBLK)
DISKIMG_FILE :=
MKFS_EXT4 ?= mkfs.ext4
ifeq ($(call has, VIRTIOBLK), 1)
    OBJS_EXTRA += virtio-blk.o
    DISKIMG_FILE := ext4.img
    OPTS += -d $(DISKIMG_FILE)
    MKFS_EXT4 := $(shell which $(MKFS_EXT4))
    ifndef MKFS_EXT4
	MKFS_EXT4 := $(shell which $$(brew --prefix e2fsprogs)/sbin/mkfs.ext4)
    endif
    ifndef MKFS_EXT4
        $(error "No mkfs.ext4 found.")
    endif
endif

# virtio-rng
ENABLE_VIRTIORNG ?= 1
$(call set-feature, VIRTIORNG)
ifeq ($(call has, VIRTIORNG), 1)
    OBJS_EXTRA += virtio-rng.o
endif

# virtio-fs
ENABLE_VIRTIOFS ?= 1
$(call set-feature, VIRTIOFS)
SHARED_DIRECTORY ?= ./shared
ifeq ($(call has, VIRTIOFS), 1)
    OBJS_EXTRA += virtio-fs.o
    OPTS += -s $(SHARED_DIRECTORY)
endif

NETDEV ?= tap
# virtio-net
ENABLE_VIRTIONET ?= 1
ifeq ($(UNAME_S),Darwin)
    # macOS: auto-select backend when using default TAP setting
    ifeq ($(NETDEV),tap)
        NETDEV :=
    endif
else ifneq ($(UNAME_S),Linux)
    # Other platforms: disable virtio-net
    ENABLE_VIRTIONET := 0
endif

$(call set-feature, VIRTIONET)
ifeq ($(call has, VIRTIONET), 1)
    OBJS_EXTRA += virtio-net.o
    OBJS_EXTRA += netdev.o

    ifeq ($(UNAME_S),Darwin)
        # macOS: support both vmnet and user (slirp) backends
        OBJS_EXTRA += netdev-vmnet.o
        OBJS_EXTRA += slirp.o
        CFLAGS += -fblocks
        LDFLAGS += -framework vmnet
    else
        # Linux: use tap/slirp backends
        OBJS_EXTRA += slirp.o
    endif
endif

# virtio-snd
ENABLE_VIRTIOSND ?= 1
ifneq ($(UNAME_S),$(filter $(UNAME_S),Linux Darwin))
    ENABLE_VIRTIOSND := 0
endif

ifeq ($(UNAME_S),Linux)
    # Check ALSA installation
    ifeq (1, $(call check-alsa))
        $(warning No libasound installed. Check libasound in advance.)
        ENABLE_VIRTIOSND := 0
    endif
endif
ifeq ($(UNAME_S),Darwin)
    ifeq (1, $(call check-coreaudio))
        $(warning No CoreAudio installed Check AudioToolbox in advance.)
        ENABLE_VIRTIOSND := 0
    endif
endif
$(call set-feature, VIRTIOSND)
ifeq ($(call has, VIRTIOSND), 1)
    OBJS_EXTRA += virtio-snd.o

    PA_LIB := portaudio/lib/.libs/libportaudio.a
    PA_CFLAGS := -Iportaudio/include
    PA_CONFIG_PARAMS :=
    LDFLAGS += $(PA_LIB)
    CFLAGS += $(PA_CFLAGS)

    ifeq ($(UNAME_S),Linux)
        LDFLAGS += -lasound -lrt
        PA_CONFIG_PARAMS += --with-alsa
        # Check PulseAudio installation
        ifeq (0, $(call check-pa))
            LDFLAGS += -lpulse
            PA_CONFIG_PARAMS += --with-pulseaudio
        endif
        ifeq (0, $(call check-jack2))
            LDFLAGS += -ljack
            PA_CONFIG_PARAMS += --with-jack
        endif
    endif
    ifeq ($(UNAME_S),Darwin)
        LDFLAGS += -framework CoreServices -framework CoreFoundation -framework AudioUnit -framework AudioToolbox -framework CoreAudio
    endif

    # PortAudio requires libm, yet we set -lm in the end of LDFLAGS
    # so that the other libraries will be benefited for no need to set
    # -lm separately.
    LDFLAGS += -lpthread

portaudio/Makefile:
	git submodule update --init portaudio
$(PA_LIB): portaudio/Makefile
	cd $(dir $<) && git clean -fdx && git reset --hard HEAD
	cd $(dir $<) && LDFLAGS="" ./configure \
        --enable-static \
        --disable-shared \
        --without-samples \
        --without-tests \
        --without-oss \
        --without-sndio \
        --disable-dependency-tracking \
        $(PA_CONFIG_PARAMS)
	$(MAKE) -C $(dir $<)
main.o: $(PA_LIB)
virtio-snd.o: $(PA_LIB)
# suppress warning when compiling PortAudio
virtio-snd.o: CFLAGS += -Wno-unused-parameter
endif

# Set libm as the last dependency so that no need to set -lm seperately.
LDFLAGS += -lm

# .DEFAULT_GOAL should be set to all since the very first target is not all
# after git submodule.
.DEFAULT_GOAL := all

BIN = semu
all: $(BIN) minimal.dtb

OBJS := \
	riscv.o \
	ram.o \
	utils.o \
	plic.o \
	uart.o \
	main.o \
	aclint.o \
	coro.o \
	$(OBJS_EXTRA)

deps := $(OBJS:%.o=.%.o.d)

GDBSTUB_LIB := mini-gdbstub/build/libgdbstub.a
LDFLAGS += $(GDBSTUB_LIB)
mini-gdbstub/Makefile:
	git submodule update --init $(dir $@)
$(GDBSTUB_LIB): mini-gdbstub/Makefile
	$(MAKE) -C $(dir $<)
$(OBJS): $(GDBSTUB_LIB)

ifeq ($(call has, VIRTIONET), 1)
MINISLIRP_DIR := minislirp
MINISLIRP_LIB := minislirp/src/libslirp.a
LDFLAGS += $(MINISLIRP_LIB)
# macOS: workaround for swab redefinition and add resolv library
MINISLIRP_CFLAGS :=
ifeq ($(UNAME_S),Darwin)
    MINISLIRP_CFLAGS := MYCFLAGS="-D_DARWIN_C_SOURCE"
    LDFLAGS += -lresolv
endif
$(MINISLIRP_DIR)/src/Makefile:
	git submodule update --init $(MINISLIRP_DIR)
$(MINISLIRP_LIB): $(MINISLIRP_DIR)/src/Makefile
	$(MAKE) -C $(dir $<) $(MINISLIRP_CFLAGS)
$(OBJS): $(MINISLIRP_LIB)
endif

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

DTC ?= dtc

# GNU Make treats the space character as a separator. The only way to handle
# filtering a pattern with space characters in a Makefile is by replacing spaces
# with another character that is guaranteed not to appear in the variable value.
# For instance, one can choose a character like '^' that is known not to be
# present in the variable value.
# Reference: https://stackoverflow.com/questions/40886386
E :=
S := $E $E

# During boot process, the emulator manually manages the growth of ticks to
# suppress RCU CPU stall warnings. Thus, we need an target time to set the
# increment of ticks. According to Using RCU’s CPU Stall Detector[1], the
# grace period for RCU CPU stalls is typically set to 21 seconds.
# By dividing this value by two as the expected completion time, we can
# provide a sufficient buffer to reduce the impact of errors and avoid
# RCU CPU stall warnings.
# [1] docs.kernel.org/RCU/stallwarn.html#config-rcu-cpu-stall-timeout
CFLAGS += -D SEMU_BOOT_TARGET_TIME=10

SMP ?= 1

# Track SMP value changes to force DTB regeneration
.smp_stamp: FORCE
	@if [ ! -f .smp_stamp ] || [ "$$(cat .smp_stamp 2>/dev/null)" != "$(SMP)" ]; then \
	    echo "$(SMP)" > .smp_stamp; \
	    rm -f riscv-harts.dtsi minimal.dtb; \
	fi

.PHONY: riscv-harts.dtsi
riscv-harts.dtsi: .smp_stamp
	$(Q)python3 scripts/gen-hart-dts.py $@ $(SMP) $(CLOCK_FREQ)

minimal.dtb: minimal.dts riscv-harts.dtsi
	$(VECHO) " DTC\t$@\n"
	$(Q)$(RM) $@
	$(Q)$(CC) -nostdinc -E -P -x assembler-with-cpp -undef \
	    $(DT_CFLAGS) \
	    $(subst ^,$S,$(filter -D^SEMU_FEATURE_%, $(subst -D$(S)SEMU_FEATURE,-D^SEMU_FEATURE,$(CFLAGS)))) $< \
	    | $(DTC) - > $@

.PHONY: FORCE
FORCE:

# Rules for downloading prebuilt Linux kernel image
include mk/external.mk

ext4.img:
	$(Q)dd if=/dev/zero of=$@ bs=4k count=600
	$(Q)$(MKFS_EXT4) -F $@

.PHONY: $(DIRECTORY)
$(SHARED_DIRECTORY):
	@if [ ! -d $@ ]; then \
		echo "Creating mount directory: $@"; \
		mkdir -p $@; \
	fi

check: $(BIN) minimal.dtb $(KERNEL_DATA) $(INITRD_DATA) $(DISKIMG_FILE) $(SHARED_DIRECTORY)
	@$(call notice, Ready to launch Linux kernel. Please be patient.)
	$(Q)./$(BIN) -k $(KERNEL_DATA) -c $(SMP) -b minimal.dtb -i $(INITRD_DATA) $(if $(NETDEV),-n $(NETDEV)) $(OPTS)

build-image:
	scripts/build-image.sh

clean:
	$(Q)$(RM) $(BIN) $(OBJS) $(deps)
	$(Q)$(MAKE) -C mini-gdbstub clean
	$(Q)if [ -n "$(MINISLIRP_DIR)" ] && [ -d "$(MINISLIRP_DIR)/src" ]; then \
		$(MAKE) -C $(MINISLIRP_DIR)/src clean; \
	fi

distclean: clean
	$(Q)$(RM) riscv-harts.dtsi
	$(Q)$(RM) minimal.dtb
	$(Q)$(RM) .smp_stamp
	$(Q)$(RM) Image rootfs.cpio
	$(Q)$(RM) ext4.img

-include $(deps)
