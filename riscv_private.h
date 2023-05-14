#pragma once

/* base RISC-V ISA */
enum {
    RV32_OP_IMM = 0b0010011,
    RV32_OP = 0b0110011,
    RV32_LUI = 0b0110111,
    RV32_AUIPC = 0b0010111,
    RV32_JAL = 0b1101111,
    RV32_JALR = 0b1100111,
    RV32_BRANCH = 0b1100011,
    RV32_LOAD = 0b0000011,
    RV32_STORE = 0b0100011,
    RV32_MISC_MEM = 0b0001111,
    RV32_SYSTEM = 0b1110011,
    RV32_AMO = 0b0101111,
};

enum {
    RV_MEM_LB = 0b000,
    RV_MEM_LH = 0b001,
    RV_MEM_LW = 0b010,
    RV_MEM_LBU = 0b100,
    RV_MEM_LHU = 0b101,
    RV_MEM_SB = 0b000,
    RV_MEM_SH = 0b001,
    RV_MEM_SW = 0b010,
};

/* RISC-V registers (mnemonics, ABI names) */
enum {
    RV_R_A0 = 10,
    RV_R_A1 = 11,
    RV_R_A6 = 16,
    RV_R_A7 = 17,
};

/* privileged ISA: CSRs */
enum {
    /* S-mode (Supervisor Trap Setup) */
    RV_CSR_SSTATUS = 0x100,    /**< Supervisor status register */
    RV_CSR_SIE = 0x104,        /**< Supervisor interrupt-enable register */
    RV_CSR_STVEC = 0x105,      /**< Supervisor trap handler base address */
    RV_CSR_SCOUNTEREN = 0x106, /**< Supervisor counter enable */

    /* S-mode (Supervisor Configuration) */
    RV_CSR_SENVCFG = 0x10A, /* Supervisor environment configuration register */

    /* S-mode (Supervisor Trap Handling) */
    RV_CSR_SSCRATCH =
        0x140,             /**< Scratch register for supervisor trap handlers */
    RV_CSR_SEPC = 0x141,   /**< Supervisor exception program counter */
    RV_CSR_SCAUSE = 0x142, /**< Supervisor trap cause */
    RV_CSR_STVAL = 0x143,  /**< Supervisor bad address or instruction */
    RV_CSR_SIP = 0x144,    /**< Supervisor interrupt pending */

    /* S-mode (Supervisor Protection and Translation) */
    RV_CSR_SATP = 0x180, /**< Supervisor address translation and protection */
};

/* privileged ISA: exception causes */
enum {
    RV_EXC_PC_MISALIGN = 0,    /**< Instruction address misaligned */
    RV_EXC_FETCH_FAULT = 1,    /**< Instruction access fault */
    RV_EXC_ILLEGAL_INSTR = 2,  /**< Illegal instruction */
    RV_EXC_BREAKPOINT = 3,     /**< Breakpoint */
    RV_EXC_LOAD_MISALIGN = 4,  /**< Load address misaligned */
    RV_EXC_LOAD_FAULT = 5,     /**< Load access fault */
    RV_EXC_STORE_MISALIGN = 6, /**< Store/AMO address misaligned */
    RV_EXC_STORE_FAULT = 7,    /**< Store/AMO access fault */
    RV_EXC_ECALL_U = 8,        /**< Environment call from U-mode */
    RV_EXC_ECALL_S = 9,        /**< Environment call from S-mode */
    /* 10–11 Reserved */
    RV_EXC_FETCH_PFAULT = 12, /**< Instruction page fault */
    RV_EXC_LOAD_PFAULT = 13,  /**< Load page fault */
    /* 14 Reserved */
    RV_EXC_STORE_PFAULT = 15, /**< Store/AMO page fault */
};

/* privileged ISA: other */
enum { RV_PAGE_SHIFT = 12, RV_PAGE_SIZE = 1 << RV_PAGE_SHIFT };

enum {
    RV_INT_SSI = 1,
    RV_INT_SSI_BIT = (1 << RV_INT_SSI),
    RV_INT_STI = 5,
    RV_INT_STI_BIT = (1 << RV_INT_STI),
    RV_INT_SEI = 9,
    RV_INT_SEI_BIT = (1 << RV_INT_SEI),
};

/* SBI 0.2 */

#define SBI_SUCCESS 0
#define SBI_ERR_NOT_SUPPORTED -2

#define SBI_EID_BASE 0x10
#define SBI_BASE__GET_SBI_SPEC_VERSION 0
#define SBI_BASE__GET_SBI_IMPL_ID 1
#define SBI_BASE__GET_SBI_IMPL_VERSION 2
#define SBI_BASE__PROBE_EXTENSION 3
#define SBI_BASE__GET_MVENDORID 4
#define SBI_BASE__GET_MARCHID 5
#define SBI_BASE__GET_MIMPID 6

#define SBI_EID_TIMER 0x54494D45
#define SBI_TIMER__SET_TIMER 0

#define SBI_EID_RST 0x53525354
#define SBI_RST__SYSTEM_RESET 0
