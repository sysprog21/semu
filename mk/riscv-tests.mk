RISCV_TESTS_DIR := tests/riscv-tests
RISCV_TESTS_ISA_DIR := $(RISCV_TESTS_DIR)/isa
RISCV_TESTS_BIN_DIR := tests/riscv-tests-data

$(RISCV_TESTS_DIR)/configure:
	$(VECHO) "Cloning riscv-tests repository...\n"
	$(Q)git submodule update --init --recursive

RISCV_TESTS_LIST := $(shell scripts/gen-isa-test-list.sh)
RISCV_TESTS_BINS := $(addprefix $(RISCV_TESTS_BIN_DIR)/, $(RISCV_TESTS_LIST))

# Build riscv-tests
# FIXME: make it more generic without specifying rv64ui-p-add
$(RISCV_TESTS_ISA_DIR)/rv64ui-p-add: $(RISCV_TESTS_DIR)/configure
ifndef CROSS_COMPILE
	$(error "GNU Toolchain for RISC-V is required. Please check package installation")
endif
	$(VECHO) "Building riscv-tests...\n"
	$(Q) cd $(RISCV_TESTS_DIR); ./configure $(REDIR)
	$(Q)$(MAKE) -C $(RISCV_TESTS_DIR) RISCV_PREFIX=$(CROSS_COMPILE) isa -j $(shell nproc) $(REDIR)

# Convert generated ELF files to raw binary
$(RISCV_TESTS_BIN_DIR)/rv64%: $(RISCV_TESTS_ISA_DIR)/rv64%
	$(Q)mkdir -p $(RISCV_TESTS_BIN_DIR)
	$(Q)$(CROSS_COMPILE)objcopy -O binary $^ $@

ifeq ("$(ENABLE_RISCV_TESTS)", "1")
run-tests: $(BIN) $(RISCV_TESTS_BINS)
	$(Q)./$(BIN) --test
endif
