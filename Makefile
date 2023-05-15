include mk/common.mk

CC ?= gcc
CFLAGS := -O2 -g -Wall -Wextra
CFLAGS += -include common.h

BIN = semu
all: $(BIN) minimal.dtb

OBJS := \
	riscv.o \
	ram.o \
	plic.o \
	uart.o \
	virtio-net.o \
	main.o

deps := $(OBJS:%.o=.%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

DTC ?= dtc

minimal.dtb: minimal.dts
	$(VECHO) " DTC\t$@\n"
	$(Q)$(DTC) $< > $@

# Rules for downloading prebuilt Linux kernel image
include mk/external.mk

check: $(BIN) minimal.dtb $(KERNEL_DATA)
	@$(call notice, Ready to launch Linux kernel. Please be patient.)
	$(Q)./$(BIN) $(KERNEL_DATA)

clean:
	$(Q)$(RM) $(BIN) $(OBJS) $(deps)

distclean: clean
	$(Q)$(RM) minimal.dtb
	$(Q)$(RM) Image

-include $(deps)
