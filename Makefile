include mk/common.mk

CC ?= gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lpthread
SHELL := /bin/bash

# For building riscv-tests
CROSS_COMPILE ?= riscv64-unknown-elf-

BIN := semu

OBJS := semu.o

# Whether to enable riscv-tests
ENABLE_RISCV_TESTS ?= 0
ifeq ("$(ENABLE_RISCV_TESTS)", "1")
CFLAGS += -DENABLE_RISCV_TESTS
OBJS += tests/isa-test.o
endif

deps := $(OBJS:%.o=%.o.d)

%.o: %.c
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

all: $(BIN)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

# Rules for downloading xv6 kernel and root file system
include mk/external.mk

check: $(BIN) $(KERNEL_DATA) $(ROOTFS_DATA)
	@$(call notice, Ready to launch xv6. Please be patient.)
	$(Q)./$(BIN) $(KERNEL_DATA) $(ROOTFS_DATA)

# unit tests for RISC-V processors
include mk/riscv-tests.mk

clean:
	$(Q)$(RM) $(BIN) $(OBJS) $(deps)
distclean: clean
	$(Q)rm -rf $(KERNEL_DATA) $(ROOTFS_DATA)
	-$(Q)$(MAKE) -C tests/riscv-tests/isa clean $(REDIR)
	$(Q)rm -rf $(RISCV_TESTS_BIN_DIR)

-include $(deps)
