include mk/common.mk

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

NETDEV ?= tap
# virtio-net
ENABLE_VIRTIONET ?= 1
ifneq ($(UNAME_S),Linux)
    ENABLE_VIRTIONET := 0
endif
$(call set-feature, VIRTIONET)
ifeq ($(call has, VIRTIONET), 1)
    OBJS_EXTRA += virtio-net.o
    OBJS_EXTRA += netdev.o
endif

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
	$(OBJS_EXTRA)

deps := $(OBJS:%.o=.%.o.d)

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

SMP ?= 1
CFLAGS += -D SEMU_BOOT_TARGET_TIME=10
.PHONY: riscv-harts.dtsi
riscv-harts.dtsi:
	$(Q)python3 scripts/gen-hart-dts.py $@ $(SMP) $(CLOCK_FREQ)

minimal.dtb: minimal.dts riscv-harts.dtsi
	$(VECHO) " DTC\t$@\n"
	$(Q)$(CC) -nostdinc -E -P -x assembler-with-cpp -undef \
	    $(DT_CFLAGS) \
	    $(subst ^,$S,$(filter -D^SEMU_FEATURE_%, $(subst -D$(S)SEMU_FEATURE,-D^SEMU_FEATURE,$(CFLAGS)))) $< \
	    | $(DTC) - > $@

# Rules for downloading prebuilt Linux kernel image
include mk/external.mk

ext4.img:
	$(Q)dd if=/dev/zero of=$@ bs=4k count=600
	$(Q)$(MKFS_EXT4) -F $@

check: $(BIN) minimal.dtb $(KERNEL_DATA) $(INITRD_DATA) $(DISKIMG_FILE)
	@$(call notice, Ready to launch Linux kernel. Please be patient.)
	$(Q)./$(BIN) -k $(KERNEL_DATA) -c $(SMP) -b minimal.dtb -i $(INITRD_DATA) -n $(NETDEV) $(OPTS)

build-image:
	scripts/build-image.sh

clean:
	$(Q)$(RM) $(BIN) $(OBJS) $(deps)

distclean: clean
	$(Q)$(RM) riscv-harts.dtsi
	$(Q)$(RM) minimal.dtb
	$(Q)$(RM) Image rootfs.cpio
	$(Q)$(RM) ext4.img 

-include $(deps)
