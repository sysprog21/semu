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

%.o: %.c
	$(CC) -o $@ $(CFLAGS) -c $<

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

kernel.bin:
	scripts/download.sh

check: all kernel.bin
	./semu kernel.bin fs.img

RISCV_TESTS_DIR := tests/riscv-tests
RISCV_TESTS_ISA_DIR := $(RISCV_TESTS_DIR)/isa
RISCV_TESTS_BIN_DIR := tests/riscv-tests-data

$(RISCV_TESTS_DIR)/configure:
	git submodule update --init --recursive

RISCV_TESTS_LIST := $(shell scripts/gen-isa-test-list.sh)
RISCV_TESTS_BINS := $(addprefix $(RISCV_TESTS_BIN_DIR)/, $(RISCV_TESTS_LIST))

# Build riscv-tests
# FIXME: make it more generic without specifying rv64ui-p-add
$(RISCV_TESTS_ISA_DIR)/rv64ui-p-add: $(RISCV_TESTS_DIR)/configure
	cd $(RISCV_TESTS_DIR); ./configure
	$(MAKE) -C $(RISCV_TESTS_DIR) isa -j$(shell nproc)

# Convert generated ELF files to raw binary
$(RISCV_TESTS_BIN_DIR)/rv64%: $(RISCV_TESTS_ISA_DIR)/rv64%
	mkdir -p $(RISCV_TESTS_BIN_DIR)
	$(CROSS_COMPILE)objcopy -O binary $^ $@

ifeq ("$(ENABLE_RISCV_TESTS)", "1")
run-tests: $(BIN) $(RISCV_TESTS_BINS)
	./semu --test
endif

clean:
	rm -f $(BIN) $(OBJS)
distclean: clean
	rm -f kernel.bin fs.img
	-$(MAKE) -C tests/riscv-tests/isa clean
	rm -rf $(RISCV_TESTS_BIN_DIR)
