#include <stdio.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Return the string representation of an error code identifier */
static const char *vm_error_str(vm_error_t err)
{
    static const char *errors[] = {
        "NONE",
        "EXCEPTION",
        "USER",
    };
    if (err >= 0 && err < ARRAY_SIZE(errors))
        return errors[err];
    return "UNKNOWN";
}

/* Return a human-readable description for a RISC-V exception cause */
static const char *vm_exc_cause_str(uint32_t err)
{
    static const char *errors[] = {
        [0] = "Instruction address misaligned",
        [1] = "Instruction access fault",
        [2] = "Illegal instruction",
        [3] = "Breakpoint",
        [4] = "Load address misaligned",
        [5] = "Load access fault",
        [6] = "Store/AMO address misaligned",
        [7] = "Store/AMO access fault",
        [8] = "Environment call from U-mode",
        [9] = "Environment call from S-mode",
        [12] = "Instruction page fault",
        [13] = "Load page fault",
        [15] = "Store/AMO page fault",
    };
    if (err < ARRAY_SIZE(errors))
        return errors[err];
    return "[Unknown]";
}

void vm_error_report(const hart_t *vm)
{
    fprintf(stderr, "vm error %s: %s. val=%#x\n", vm_error_str(vm->error),
            vm_exc_cause_str(vm->exc_cause), vm->exc_val);
}

/* Instruction decoding */

/* clang-format off */
/* instruction decode masks */
enum {
    //               ....xxxx....xxxx....xxxx....xxxx
    FR_RD        = 0b00000000000000000000111110000000,
    FR_FUNCT3    = 0b00000000000000000111000000000000,
    FR_RS1       = 0b00000000000011111000000000000000,
    FR_RS2       = 0b00000001111100000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FI_IMM_11_0  = 0b11111111111100000000000000000000, // I-type
    //               ....xxxx....xxxx....xxxx....xxxx
    FS_IMM_4_0   = 0b00000000000000000000111110000000, // S-type
    FS_IMM_11_5  = 0b11111110000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FB_IMM_11    = 0b00000000000000000000000010000000, // B-type
    FB_IMM_4_1   = 0b00000000000000000000111100000000,
    FB_IMM_10_5  = 0b01111110000000000000000000000000,
    FB_IMM_12    = 0b10000000000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
    FU_IMM_31_12 = 0b11111111111111111111000000000000, // U-type
    //               ....xxxx....xxxx....xxxx....xxxx
    FJ_IMM_19_12 = 0b00000000000011111111000000000000, // J-type
    FJ_IMM_11    = 0b00000000000100000000000000000000,
    FJ_IMM_10_1  = 0b01111111111000000000000000000000,
    FJ_IMM_20    = 0b10000000000000000000000000000000,
    //               ....xxxx....xxxx....xxxx....xxxx
};
/* clang-format on */

/* decode U-type instruction immediate */
static inline uint32_t decode_u(uint32_t insn)
{
    return insn & FU_IMM_31_12;
}

/* decode I-type instruction immediate */
static inline uint32_t decode_i(uint32_t insn)
{
    return ((int32_t) (insn & FI_IMM_11_0)) >> 20;
}

static inline uint32_t decode_j(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FJ_IMM_20);
    dst |= (insn & FJ_IMM_19_12) << 11;
    dst |= (insn & FJ_IMM_11) << 2;
    dst |= (insn & FJ_IMM_10_1) >> 9;
    /* NOTE: shifted to 2nd least significant bit */
    return ((int32_t) dst) >> 11;
}

/* decode B-type instruction immediate */
static inline uint32_t decode_b(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FB_IMM_12);
    dst |= (insn & FB_IMM_11) << 23;
    dst |= (insn & FB_IMM_10_5) >> 1;
    dst |= (insn & FB_IMM_4_1) << 12;
    /* NOTE: shifted to 2nd least significant bit */
    return ((int32_t) dst) >> 19;
}

/* decode S-type instruction immediate */
static inline uint32_t decode_s(uint32_t insn)
{
    uint32_t dst = 0;
    dst |= (insn & FS_IMM_11_5);
    dst |= (insn & FS_IMM_4_0) << 13;
    return ((int32_t) dst) >> 20;
}

static inline uint16_t decode_i_unsigned(uint32_t insn)
{
    return insn >> 20;
}

/* decode rd field */
static inline uint8_t decode_rd(uint32_t insn)
{
    return (insn & FR_RD) >> 7;
}

/* decode rs1 field */
static inline uint8_t decode_rs1(uint32_t insn)
{
    return (insn & FR_RS1) >> 15;
}

/* decode rs2 field */
static inline uint8_t decode_rs2(uint32_t insn)
{
    return (insn & FR_RS2) >> 20;
}

/* decoded funct3 field */
static inline uint8_t decode_func3(uint32_t insn)
{
    return (insn & FR_FUNCT3) >> 12;
}

/* decoded funct5 field */
static inline uint8_t decode_func5(uint32_t insn)
{
    return insn >> 27;
}

static inline uint32_t read_rs1(const hart_t *vm, uint32_t insn)
{
    return vm->x_regs[decode_rs1(insn)];
}

static inline uint32_t read_rs2(const hart_t *vm, uint32_t insn)
{
    return vm->x_regs[decode_rs2(insn)];
}

/* virtual addressing */

static void mmu_invalidate(hart_t *vm)
{
    vm->cache_fetch.n_pages = 0xFFFFFFFF;
}

/* Pre-verify the root page table to minimize page table access during
 * translation time.
 */
static void mmu_set(hart_t *vm, uint32_t satp)
{
    mmu_invalidate(vm);
    if (satp >> 31) {
        uint32_t *page_table = vm->mem_page_table(vm, satp & MASK(22));
        if (!page_table)
            return;
        vm->page_table = page_table;
        satp &= ~(MASK(9) << 22);
    } else {
        vm->page_table = NULL;
        satp = 0;
    }
    vm->satp = satp;
}

#define PTE_ITER(page_table, vpn, additional_checks)    \
    *pte = &(page_table)[vpn];                          \
    switch ((**pte) & MASK(4)) {                        \
    case 0b0001:                                        \
        break; /* pointer to next level */              \
    case 0b0011:                                        \
    case 0b0111:                                        \
    case 0b1001:                                        \
    case 0b1011:                                        \
    case 0b1111:                                        \
        *ppn = (**pte) >> 10;                           \
        additional_checks return true; /* leaf entry */ \
    case 0b0101:                                        \
    case 0b1101:                                        \
    default:                                            \
        *pte = NULL;                                    \
        return true; /* not valid */                    \
    }

/* Assume vm->page_table is set.
 *
 * If there is an error fetching a page table, return false.
 * Otherwise return true and:
 *   - in case of valid leaf: set *pte and *ppn
 *   - none found (page fault): set *pte to NULL
 */
static bool mmu_lookup(const hart_t *vm,
                       uint32_t vpn,
                       uint32_t **pte,
                       uint32_t *ppn)
{
    PTE_ITER(vm->page_table, vpn >> 10,
             if (unlikely((*ppn) & MASK(10))) /* misaligned superpage */
                 *pte = NULL;
             else *ppn |= vpn & MASK(10);)
    uint32_t *page_table = vm->mem_page_table(vm, (**pte) >> 10);
    if (!page_table)
        return false;
    PTE_ITER(page_table, vpn & MASK(10), )
    *pte = NULL;
    return true;
}

static void mmu_translate(hart_t *vm,
                          uint32_t *addr,
                          const uint32_t access_bits,
                          const uint32_t set_bits,
                          const bool skip_privilege_test,
                          const uint8_t fault,
                          const uint8_t pfault)
{
    /* NOTE: save virtual address, for physical accesses, to set exception. */
    vm->exc_val = *addr;
    if (!vm->page_table)
        return;

    uint32_t *pte_ref;
    uint32_t ppn;
    bool ok = mmu_lookup(vm, (*addr) >> RV_PAGE_SHIFT, &pte_ref, &ppn);
    if (unlikely(!ok)) {
        vm_set_exception(vm, fault, *addr);
        return;
    }

    uint32_t pte;
    if (!(pte_ref /* PTE lookup was successful */ &&
          !(ppn >> 20) /* PPN is valid */ &&
          (pte = *pte_ref, pte & access_bits) /* access type is allowed */ &&
          (!(pte & (1 << 4)) == vm->s_mode ||
           skip_privilege_test) /* privilege matches */
          )) {
        vm_set_exception(vm, pfault, *addr);
        return;
    }

    uint32_t new_pte = pte | set_bits;
    if (new_pte != pte)
        *pte_ref = new_pte;

    *addr = ((*addr) & MASK(RV_PAGE_SHIFT)) | (ppn << RV_PAGE_SHIFT);
}

static void mmu_fence(hart_t *vm, uint32_t insn UNUSED)
{
    mmu_invalidate(vm);
}

static void mmu_fetch(hart_t *vm, uint32_t addr, uint32_t *value)
{
    uint32_t vpn = addr >> RV_PAGE_SHIFT;
    if (unlikely(vpn != vm->cache_fetch.n_pages)) {
        mmu_translate(vm, &addr, (1 << 3), (1 << 6), false, RV_EXC_FETCH_FAULT,
                      RV_EXC_FETCH_PFAULT);
        if (vm->error)
            return;
        uint32_t *page_addr;
        vm->mem_fetch(vm, addr >> RV_PAGE_SHIFT, &page_addr);
        if (vm->error)
            return;
        vm->cache_fetch.n_pages = vpn;
        vm->cache_fetch.page_addr = page_addr;
    }
    *value = vm->cache_fetch.page_addr[(addr >> 2) & MASK(RV_PAGE_SHIFT - 2)];
}

static void mmu_load(hart_t *vm,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value,
                     bool reserved)
{
    mmu_translate(vm, &addr, (1 << 1) | (vm->sstatus_mxr ? (1 << 3) : 0),
                  (1 << 6), vm->sstatus_sum && vm->s_mode, RV_EXC_LOAD_FAULT,
                  RV_EXC_LOAD_PFAULT);
    if (vm->error)
        return;
    vm->mem_load(vm, addr, width, value);
    if (vm->error)
        return;

    if (unlikely(reserved)) {
        vm->lr_reservation = addr | 1;
        vm->lr_val = *value;
    }
}

static bool mmu_store(hart_t *vm,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value,
                      bool cond)
{
    mmu_translate(vm, &addr, (1 << 2), (1 << 6) | (1 << 7),
                  vm->sstatus_sum && vm->s_mode, RV_EXC_STORE_FAULT,
                  RV_EXC_STORE_PFAULT);
    if (vm->error)
        return false;

    if (unlikely(cond)) {
        uint32_t cas_value;
        vm->mem_load(vm, addr, width, &cas_value);
        if ((vm->lr_reservation != (addr | 1)) || vm->lr_val != cas_value)
            return false;

        vm->lr_reservation = 0;
    } else {
        if (unlikely(vm->lr_reservation & 1) &&
            (vm->lr_reservation & ~3) == (addr & ~3))
            vm->lr_reservation = 0;
    }
    vm->mem_store(vm, addr, width, value);
    return true;
}

/* exceptions, traps, interrupts */

void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val)
{
    vm->error = ERR_EXCEPTION;
    vm->exc_cause = cause;
    vm->exc_val = val;
}

void hart_trap(hart_t *vm)
{
    /* Fill exception fields */
    vm->scause = vm->exc_cause;
    vm->stval = vm->exc_val;

    /* Save to stack */
    vm->sstatus_spie = vm->sstatus_sie;
    vm->sstatus_spp = vm->s_mode;
    vm->sepc = vm->current_pc;

    /* Set */
    vm->sstatus_sie = false;
    mmu_invalidate(vm);
    vm->s_mode = true;
    vm->pc = vm->stvec_addr;
    if (vm->stvec_vectored)
        vm->pc += (vm->scause & MASK(31)) * 4;

    vm->error = ERR_NONE;
}

static void op_sret(hart_t *vm)
{
    /* Restore from stack */
    vm->pc = vm->sepc;
    mmu_invalidate(vm);
    vm->s_mode = vm->sstatus_spp;
    vm->sstatus_sie = vm->sstatus_spie;

    /* Reset stack */
    vm->sstatus_spp = false;
    vm->sstatus_spie = true;
}

static void op_privileged(hart_t *vm, uint32_t insn)
{
    if ((insn >> 25) == 0b0001001 /* PRIV: SFENCE_VMA */) {
        mmu_fence(vm, insn);
        return;
    }
    if (insn & ((MASK(5) << 7) | (MASK(5) << 15))) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
    switch (decode_i_unsigned(insn)) {
    case 0b000000000001: /* PRIV_EBREAK */
        vm_set_exception(vm, RV_EXC_BREAKPOINT, vm->current_pc);
        break;
    case 0b000000000000: /* PRIV_ECALL */
        vm_set_exception(vm, vm->s_mode ? RV_EXC_ECALL_S : RV_EXC_ECALL_U, 0);
        break;
    case 0b000100000010: /* PRIV_SRET */
        op_sret(vm);
        break;
    case 0b000100000101: /* PRIV_WFI */
                         /* TODO: Implement this */
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}

/* CSR instructions */

static inline void set_dest(hart_t *vm, uint32_t insn, uint32_t x)
{
    uint8_t rd = decode_rd(insn);
    if (rd)
        vm->x_regs[rd] = x;
}

/* clang-format off */
#define SIE_MASK (RV_INT_SEI_BIT | RV_INT_STI_BIT | RV_INT_SSI_BIT)
#define SIP_MASK (0              | 0              | RV_INT_SSI_BIT)
/* clang-format on */

static void csr_read(hart_t *vm, uint16_t addr, uint32_t *value)
{
    switch (addr) {
    case RV_CSR_TIME:
        *value = vm->time;
        return;
    case RV_CSR_TIMEH:
        *value = vm->time >> 32;
        return;
    case RV_CSR_INSTRET:
        *value = vm->instret;
        return;
    case RV_CSR_INSTRETH:
        *value = vm->instret >> 32;
        return;
    default:
        break;
    }

    if (!vm->s_mode) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    switch (addr) {
    case RV_CSR_SSTATUS:
        *value = 0;
        vm->sstatus_sie && (*value |= 1 << (1));
        vm->sstatus_spie && (*value |= 1 << (5));
        vm->sstatus_spp && (*value |= 1 << (8));
        vm->sstatus_sum && (*value |= 1 << (18));
        vm->sstatus_mxr && (*value |= 1 << (19));
        break;
    case RV_CSR_SIE:
        *value = vm->sie;
        break;
    case RV_CSR_SIP:
        *value = vm->sip;
        break;
    case RV_CSR_STVEC:
        *value = 0;
        *value = vm->stvec_addr;
        vm->stvec_vectored && (*value |= 1 << (0));
        break;
    case RV_CSR_SATP:
        *value = vm->satp;
        break;
    case RV_CSR_SCOUNTEREN:
        *value = vm->scounteren;
        break;
    case RV_CSR_SSCRATCH:
        *value = vm->sscratch;
        break;
    case RV_CSR_SEPC:
        *value = vm->sepc;
        break;
    case RV_CSR_SCAUSE:
        *value = vm->scause;
        break;
    case RV_CSR_STVAL:
        *value = vm->stval;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}

static void csr_write(hart_t *vm, uint16_t addr, uint32_t value)
{
    if (!vm->s_mode) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    switch (addr) {
    case RV_CSR_SSTATUS:
        vm->sstatus_sie = (value & (1 << (1))) != 0;
        vm->sstatus_spie = (value & (1 << (5))) != 0;
        vm->sstatus_spp = (value & (1 << (8))) != 0;
        vm->sstatus_sum = (value & (1 << (18))) != 0;
        vm->sstatus_mxr = (value & (1 << (19))) != 0;
        break;
    case RV_CSR_SIE:
        value &= SIE_MASK;
        vm->sie = value;
        break;
    case RV_CSR_SIP:
        value &= SIP_MASK;
        value |= vm->sip & ~SIP_MASK;
        vm->sip = value;
        break;
    case RV_CSR_STVEC:
        vm->stvec_addr = value;
        vm->stvec_addr &= ~0b11;
        vm->stvec_vectored = (value & (1 << (0))) != 0;
        break;
    case RV_CSR_SATP:
        mmu_set(vm, value);
        break;
    case RV_CSR_SCOUNTEREN:
        vm->scounteren = value;
        break;
    case RV_CSR_SSCRATCH:
        vm->sscratch = value;
        break;
    case RV_CSR_SEPC:
        vm->sepc = value;
        break;
    case RV_CSR_SCAUSE:
        vm->scause = value;
        break;
    case RV_CSR_STVAL:
        vm->stval = value;
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    }
}

static void op_csr_rw(hart_t *vm, uint32_t insn, uint16_t csr, uint32_t wvalue)
{
    if (decode_rd(insn)) {
        uint32_t value;
        csr_read(vm, csr, &value);
        if (unlikely(vm->error))
            return;
        set_dest(vm, insn, value);
    }
    csr_write(vm, csr, wvalue);
}

static void op_csr_cs(hart_t *vm,
                      uint32_t insn,
                      uint16_t csr,
                      uint32_t setmask,
                      uint32_t clearmask)
{
    uint32_t value;
    csr_read(vm, csr, &value);
    if (unlikely(vm->error))
        return;
    set_dest(vm, insn, value);
    if (decode_rs1(insn))
        csr_write(vm, csr, (value & ~clearmask) | setmask);
}

static void op_system(hart_t *vm, uint32_t insn)
{
    switch (decode_func3(insn)) {
    /* CSR */
    case 0b001: /* CSRRW */
        op_csr_rw(vm, insn, decode_i_unsigned(insn), read_rs1(vm, insn));
        break;
    case 0b101: /* CSRRWI */
        op_csr_rw(vm, insn, decode_i_unsigned(insn), decode_rs1(insn));
        break;
    case 0b010: /* CSRRS */
        op_csr_cs(vm, insn, decode_i_unsigned(insn), read_rs1(vm, insn), 0);
        break;
    case 0b110: /* CSRRSI */
        op_csr_cs(vm, insn, decode_i_unsigned(insn), decode_rs1(insn), 0);
        break;
    case 0b011: /* CSRRC */
        op_csr_cs(vm, insn, decode_i_unsigned(insn), 0, read_rs1(vm, insn));
        break;
    case 0b111: /* CSRRCI */
        op_csr_cs(vm, insn, decode_i_unsigned(insn), 0, decode_rs1(insn));
        break;

    /* privileged instruction */
    case 0b000: /* SYS_PRIV */
        op_privileged(vm, insn);
        break;

    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

/* Unprivileged instructions */

static uint32_t op_mul(uint32_t insn, uint32_t a, uint32_t b)
{
    /* TODO: Test ifunc7 zeros */
    switch (decode_func3(insn)) {
    case 0b000: { /* MUL */
        const int64_t _a = (int32_t) a;
        const int64_t _b = (int32_t) b;
        return ((uint64_t) (_a * _b)) & ((1ULL << 32) - 1);
    }
    case 0b001: { /* MULH */
        const int64_t _a = (int32_t) a;
        const int64_t _b = (int32_t) b;
        return ((uint64_t) (_a * _b)) >> 32;
    }
    case 0b010: { /* MULHSU */
        const int64_t _a = (int32_t) a;
        const uint64_t _b = b;
        return ((uint64_t) (_a * _b)) >> 32;
    }
    case 0b011: /* MULHU */
        return (uint32_t) ((((uint64_t) a) * ((uint64_t) b)) >> 32);
    case 0b100: /* DIV */
        return b ? (a == 0x80000000 && (int32_t) b == -1)
                       ? 0x80000000
                       : (uint32_t) (((int32_t) a) / ((int32_t) b))
                 : 0xFFFFFFFF;
    case 0b101: /* DIVU */
        return b ? (a / b) : 0xFFFFFFFF;
    case 0b110: /* REM */
        return b ? (a == 0x80000000 && (int32_t) b == -1)
                       ? 0
                       : (uint32_t) (((int32_t) a) % ((int32_t) b))
                 : a;
    case 0b111: /* REMU */
        return b ? (a % b) : a;
    }
    __builtin_unreachable();
}

#define NEG_BIT (insn & (1 << 30))
static uint32_t op_rv32i(uint32_t insn, bool is_reg, uint32_t a, uint32_t b)
{
    /* TODO: Test ifunc7 zeros */
    switch (decode_func3(insn)) {
    case 0b000: /* IFUNC_ADD */
        return a + ((is_reg && NEG_BIT) ? -b : b);
    case 0b010: /* IFUNC_SLT */
        return ((int32_t) a) < ((int32_t) b);
    case 0b011: /* IFUNC_SLTU */
        return a < b;
    case 0b100: /* IFUNC_XOR */
        return a ^ b;
    case 0b110: /* IFUNC_OR */
        return a | b;
    case 0b111: /* IFUNC_AND */
        return a & b;
    case 0b001: /* IFUNC_SLL */
        return a << (b & MASK(5));
    case 0b101: /* IFUNC_SRL */
        return NEG_BIT ? (uint32_t) (((int32_t) a) >> (b & MASK(5))) /* SRA */
                       : a >> (b & MASK(5)) /* SRL */;
    }
    __builtin_unreachable();
}
#undef NEG_BIT

static bool op_jmp(hart_t *vm, uint32_t insn, uint32_t a, uint32_t b)
{
    switch (decode_func3(insn)) {
    case 0b000: /* BFUNC_BEQ */
        return a == b;
    case 0b001: /* BFUNC_BNE */
        return a != b;
    case 0b110: /* BFUNC_BLTU */
        return a < b;
    case 0b111: /* BFUNC_BGEU */
        return a >= b;
    case 0b100: /* BFUNC_BLT */
        return ((int32_t) a) < ((int32_t) b);
    case 0b101: /* BFUNC_BGE */
        return ((int32_t) a) >= ((int32_t) b);
    }
    vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    return false;
}

static void do_jump(hart_t *vm, uint32_t addr)
{
    if (unlikely(addr & 0b11))
        vm_set_exception(vm, RV_EXC_PC_MISALIGN, addr);
    else
        vm->pc = addr;
}

static void op_jump_link(hart_t *vm, uint32_t insn, uint32_t addr)
{
    if (unlikely(addr & 0b11)) {
        vm_set_exception(vm, RV_EXC_PC_MISALIGN, addr);
    } else {
        set_dest(vm, insn, vm->pc);
        vm->pc = addr;
    }
}

#define AMO_OP(STORED_EXPR)                                   \
    do {                                                      \
        value2 = read_rs2(vm, insn);                          \
        mmu_load(vm, addr, RV_MEM_LW, &value, false);         \
        if (vm->error)                                        \
            return;                                           \
        set_dest(vm, insn, value);                            \
        mmu_store(vm, addr, RV_MEM_SW, (STORED_EXPR), false); \
    } while (0)

static void op_amo(hart_t *vm, uint32_t insn)
{
    if (unlikely(decode_func3(insn) != 0b010 /* amo.w */))
        return vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    uint32_t addr = read_rs1(vm, insn);
    uint32_t value, value2;
    switch (decode_func5(insn)) {
    case 0b00010: /* AMO_LR */
        if (addr & 0b11)
            return vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, addr);
        if (decode_rs2(insn))
            return vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        mmu_load(vm, addr, RV_MEM_LW, &value, true);
        if (vm->error)
            return;
        set_dest(vm, insn, value);
        break;
    case 0b00011: /* AMO_SC */
        if (addr & 0b11)
            return vm_set_exception(vm, RV_EXC_STORE_MISALIGN, addr);
        bool ok = mmu_store(vm, addr, RV_MEM_SW, read_rs2(vm, insn), true);
        if (vm->error)
            return;
        set_dest(vm, insn, ok ? 0 : 1);
        break;

    case 0b00001: /* AMOSWAP */
        AMO_OP(value2);
        break;
    case 0b00000: /* AMOADD */
        AMO_OP(value + value2);
        break;
    case 0b00100: /* AMOXOR */
        AMO_OP(value ^ value2);
        break;
    case 0b01100: /* AMOAND */
        AMO_OP(value & value2);
        break;
    case 0b01000: /* AMOOR */
        AMO_OP(value | value2);
        break;
    case 0b10000: /* AMOMIN */
        AMO_OP(((int32_t) value) < ((int32_t) value2) ? value : value2);
        break;
    case 0b10100: /* AMOMAX */
        AMO_OP(((int32_t) value) > ((int32_t) value2) ? value : value2);
        break;
    case 0b11000: /* AMOMINU */
        AMO_OP(value < value2 ? value : value2);
        break;
    case 0b11100: /* AMOMAXU */
        AMO_OP(value > value2 ? value : value2);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void vm_init(hart_t *vm)
{
    mmu_invalidate(vm);
}

#define PRIV(x) ((emu_state_t *) x->priv)
void vm_step(hart_t *vm)
{
    if (vm->hsm_status != SBI_HSM_STATE_STARTED)
        return;

    if (unlikely(vm->error))
        return;

    vm->current_pc = vm->pc;
    if ((vm->sstatus_sie || !vm->s_mode) && (vm->sip & vm->sie)) {
        uint32_t applicable = (vm->sip & vm->sie);
        uint8_t idx = ffs(applicable) - 1;
        if (idx == 1) {
            emu_state_t *data = PRIV(vm);
            data->clint.msip[vm->mhartid] = 0;
        }
        vm->exc_cause = (1U << 31) | idx;
        vm->stval = 0;
        hart_trap(vm);
    }

    uint32_t insn;
    mmu_fetch(vm, vm->pc, &insn);
    if (unlikely(vm->error))
        return;

    vm->pc += 4;
    /* Assume no integer overflow */
    vm->instret++;

    uint32_t insn_opcode = insn & MASK(7), value;
    switch (insn_opcode) {
    case RV32_OP_IMM:
        set_dest(vm, insn,
                 op_rv32i(insn, false, read_rs1(vm, insn), decode_i(insn)));
        break;
    case RV32_OP:
        if (!(insn & (1 << 25)))
            set_dest(
                vm, insn,
                op_rv32i(insn, true, read_rs1(vm, insn), read_rs2(vm, insn)));
        else
            set_dest(vm, insn,
                     op_mul(insn, read_rs1(vm, insn), read_rs2(vm, insn)));
        break;
    case RV32_LUI:
        set_dest(vm, insn, decode_u(insn));
        break;
    case RV32_AUIPC:
        set_dest(vm, insn, decode_u(insn) + vm->current_pc);
        break;
    case RV32_JAL:
        op_jump_link(vm, insn, decode_j(insn) + vm->current_pc);
        break;
    case RV32_JALR:
        op_jump_link(vm, insn, (decode_i(insn) + read_rs1(vm, insn)) & ~1);
        break;
    case RV32_BRANCH:
        if (op_jmp(vm, insn, read_rs1(vm, insn), read_rs2(vm, insn)))
            do_jump(vm, decode_b(insn) + vm->current_pc);
        break;
    case RV32_LOAD:
        mmu_load(vm, read_rs1(vm, insn) + decode_i(insn), decode_func3(insn),
                 &value, false);
        if (unlikely(vm->error))
            return;
        set_dest(vm, insn, value);
        break;
    case RV32_STORE:
        mmu_store(vm, read_rs1(vm, insn) + decode_s(insn), decode_func3(insn),
                  read_rs2(vm, insn), false);
        if (unlikely(vm->error))
            return;
        break;
    case RV32_MISC_MEM:
        switch (decode_func3(insn)) {
        case 0b000: /* MM_FENCE */
        case 0b001: /* MM_FENCE_I */
                    /* TODO: implement for multi-threading */
            break;
        default:
            vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
            break;
        }
        break;
    case RV32_AMO:
        op_amo(vm, insn);
        break;
    case RV32_SYSTEM:
        op_system(vm, insn);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}
