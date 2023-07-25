include mk/common.mk

CC ?= gcc
CFLAGS := -O2 -g -Wall -Wextra
CFLAGS += -include common.h

OBJS_EXTRA :=

# virtio-blk
ENABLE_VIRTIOBLK ?= 1
OBJS_EXTRA += virtio-blk.o
$(call set-feature, VIRTIOBLK)

# virtio-net
ENABLE_VIRTIONET ?= 1
ifeq ($(UNAME_S),Linux)
OBJS_EXTRA += virtio-net.o
else
ENABLE_VIRTIONET := 0
endif
$(call set-feature, VIRTIONET)

BIN = semu
all: $(BIN) minimal.dtb

OBJS := \
	riscv.o \
	ram.o \
	plic.o \
	uart.o \
	main.o \
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
minimal.dtb: minimal.dts
	$(VECHO) " DTC\t$@\n"
	$(Q)$(CC) -nostdinc -E -P -x assembler-with-cpp -undef \
	    $(subst ^,$S,$(filter -D^SEMU_FEATURE_%, $(subst -D$(S)SEMU_FEATURE,-D^SEMU_FEATURE,$(CFLAGS)))) $< \
	    | $(DTC) - > $@

# Rules for downloading prebuilt Linux kernel image
include mk/external.mk

ext4.img:
	$(Q)dd if=/dev/zero of=$@ bs=4k count=600
	$(Q)mkfs.ext4 -F $@

check: $(BIN) minimal.dtb $(KERNEL_DATA) ext4.img
	@$(call notice, Ready to launch Linux kernel. Please be patient.)
	$(Q)./$(BIN) -k $(KERNEL_DATA) -b minimal.dtb -d ext4.img

build-image:
	scripts/build-image.sh

clean:
	$(Q)$(RM) $(BIN) $(OBJS) $(deps)

distclean: clean
	$(Q)$(RM) minimal.dtb
	$(Q)$(RM) Image
	$(Q)$(RM) ext4.img 

-include $(deps)
