#include <stdio.h>
#include <string.h>

#include "common.h"
#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

#if !defined(__GNUC__) && !defined(__clang__)
/* Portable parity implementation for non-GCC/Clang compilers */
static inline unsigned int __builtin_parity(unsigned int x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1;
}
#endif

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

enum {
    DECODE_OPCODE_SHIFT = 0,
    DECODE_RD_SHIFT = 7,
    DECODE_RS1_SHIFT = 12,
    DECODE_RS2_SHIFT = 17,
    DECODE_FUNCT3_SHIFT = 22,
    DECODE_FUNCT7_SHIFT = 25,
};

static inline uint8_t decoded_opcode(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_OPCODE_SHIFT) & MASK(7);
}

static inline uint8_t decoded_rd(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_RD_SHIFT) & MASK(5);
}

static inline uint8_t decoded_rs1(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_RS1_SHIFT) & MASK(5);
}

static inline uint8_t decoded_rs2(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_RS2_SHIFT) & MASK(5);
}

static inline uint8_t decoded_funct3(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_FUNCT3_SHIFT) & MASK(3);
}

static inline uint8_t decoded_funct7(const decoded_insn_t *decoded)
{
    return (decoded->fields >> DECODE_FUNCT7_SHIFT) & MASK(7);
}

static inline uint8_t decoded_funct5(const decoded_insn_t *decoded)
{
    return decoded_funct7(decoded) >> 2;
}

static inline void decode_insn(decoded_insn_t *decoded, uint32_t insn)
{
    decoded->fields = ((uint32_t) (insn & MASK(7)) << DECODE_OPCODE_SHIFT) |
                      ((uint32_t) decode_rd(insn) << DECODE_RD_SHIFT) |
                      ((uint32_t) decode_rs1(insn) << DECODE_RS1_SHIFT) |
                      ((uint32_t) decode_rs2(insn) << DECODE_RS2_SHIFT) |
                      ((uint32_t) decode_func3(insn) << DECODE_FUNCT3_SHIFT) |
                      ((uint32_t) (insn >> 25) << DECODE_FUNCT7_SHIFT);

    switch (decoded_opcode(decoded)) {
    case RV32_OP_IMM:
    case RV32_JALR:
    case RV32_LOAD:
        decoded->imm = decode_i(insn);
        break;
    case RV32_SYSTEM:
        decoded->imm = decode_i_unsigned(insn);
        break;
    case RV32_STORE:
        decoded->imm = decode_s(insn);
        break;
    case RV32_BRANCH:
        decoded->imm = decode_b(insn);
        break;
    case RV32_JAL:
        decoded->imm = decode_j(insn);
        break;
    case RV32_LUI:
    case RV32_AUIPC:
        decoded->imm = decode_u(insn);
        break;
    default:
        decoded->imm = 0;
        break;
    }
}

static inline void icache_invalidate_all(hart_t *vm)
{
    memset(&vm->icache, 0, sizeof(vm->icache));
    vm->seq_fetch_block = NULL;
    vm->seq_fetch_next_pc = 0xFFFFFFFF;
}

/* virtual addressing */

void mmu_invalidate(hart_t *vm)
{
    vm->cache_fetch[0].n_pages = 0xFFFFFFFF;
    vm->cache_fetch[1].n_pages = 0xFFFFFFFF;
    vm->cache_fetch[0].page_addr = NULL;
    vm->cache_fetch[1].page_addr = NULL;
    /* Invalidate all 8 sets × 2 ways for load cache */
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++)
            vm->cache_load[set].ways[way].n_pages = 0xFFFFFFFF;
        vm->cache_load[set].lru = 0; /* Reset LRU to way 0 */
    }
    /* Invalidate all 8 sets × 2 ways for store cache */
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++)
            vm->cache_store[set].ways[way].n_pages = 0xFFFFFFFF;
        vm->cache_store[set].lru = 0; /* Reset LRU to way 0 */
    }
    vm->cache_load_last_vpn = 0xFFFFFFFF;
    vm->cache_store_last_vpn = 0xFFFFFFFF;
    icache_invalidate_all(vm);
    vm->ram_load_last_page = 0xFFFFFFFF;
    vm->ram_store_last_page = 0xFFFFFFFF;
    vm->ram_load_last_ptr = NULL;
    vm->ram_store_last_ptr = NULL;
}

/* Invalidate MMU caches for a specific virtual address range.
 * If size is 0 or -1, invalidate all caches (equivalent to mmu_invalidate()).
 * Otherwise, only invalidate cache entries whose VPN falls within
 * [start_addr >> PAGE_SHIFT, (start_addr + size - 1) >> PAGE_SHIFT].
 */
void mmu_invalidate_range(hart_t *vm, uint32_t start_addr, uint32_t size)
{
    /* SBI spec: size == 0 or size == -1 means flush entire address space */
    if (size == 0 || size == (uint32_t) -1) {
        mmu_invalidate(vm);
        return;
    }

    /* Calculate VPN range: [start_vpn, end_vpn] inclusive.
     * Use 64-bit arithmetic to prevent overflow when (start_addr + size - 1)
     * exceeds UINT32_MAX. For example:
     *   start_addr = 0xFFF00000, size = 0x00200000
     *   32-bit: 0xFFF00000 + 0x00200000 - 1 = 0x000FFFFF (wraps)
     *   64-bit: 0xFFF00000 + 0x00200000 - 1 = 0x100FFFFF (correct)
     * Clamp to RV32 address space maximum before calculating end_vpn.
     */
    uint32_t start_vpn = start_addr >> RV_PAGE_SHIFT;
    uint64_t end_addr = (uint64_t) start_addr + size - 1;
    if (end_addr > UINT32_MAX)
        end_addr = UINT32_MAX;
    uint32_t end_vpn = (uint32_t) end_addr >> RV_PAGE_SHIFT;

    /* Invalidate fetch cache: 2 entry */
    for (int i = 0; i < 2; i++) {
        if (vm->cache_fetch[i].n_pages >= start_vpn &&
            vm->cache_fetch[i].n_pages <= end_vpn) {
            vm->cache_fetch[i].n_pages = 0xFFFFFFFF;
            vm->cache_fetch[i].page_addr = NULL;
        }
    }

    /* Invalidate I-cache: 256 blocks */
    for (int i = 0; i < ICACHE_BLOCKS; i++) {
        icache_block_t *blk = &vm->icache.block[i];
        if (!blk->valid)
            continue;

        uint32_t icache_vpn = (blk->tag << ICACHE_INDEX_BITS) | i;
        icache_vpn >>= (RV_PAGE_SHIFT - ICACHE_OFFSET_BITS);
        if (icache_vpn >= start_vpn && icache_vpn <= end_vpn)
            blk->valid = false;
    }

    /* Invalidate load cache: 8 sets × 2 ways */
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++) {
            if (vm->cache_load[set].ways[way].n_pages >= start_vpn &&
                vm->cache_load[set].ways[way].n_pages <= end_vpn)
                vm->cache_load[set].ways[way].n_pages = 0xFFFFFFFF;
        }
    }

    /* Invalidate store cache: 8 sets × 2 ways */
    for (int set = 0; set < 8; set++) {
        for (int way = 0; way < 2; way++) {
            if (vm->cache_store[set].ways[way].n_pages >= start_vpn &&
                vm->cache_store[set].ways[way].n_pages <= end_vpn)
                vm->cache_store[set].ways[way].n_pages = 0xFFFFFFFF;
        }
    }

    /* Invalidate last-VPN fast-path caches */
    if (vm->cache_load_last_vpn >= start_vpn &&
        vm->cache_load_last_vpn <= end_vpn)
        vm->cache_load_last_vpn = 0xFFFFFFFF;
    if (vm->cache_store_last_vpn >= start_vpn &&
        vm->cache_store_last_vpn <= end_vpn)
        vm->cache_store_last_vpn = 0xFFFFFFFF;
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
    uint32_t ppn = 0; /* Initialize to avoid undefined behavior */
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
    if (likely(addr == vm->seq_fetch_next_pc && vm->seq_fetch_block != NULL)) {
        icache_block_t *seq = vm->seq_fetch_block;
        uint32_t ofs = addr & ICACHE_BLOCK_MASK;

        if (likely(seq->valid && seq->tag == (addr >> (ICACHE_OFFSET_BITS +
                                                       ICACHE_INDEX_BITS)))) {
#ifdef MMU_CACHE_STATS
            uint32_t vpn = addr >> RV_PAGE_SHIFT;
            uint32_t index = __builtin_parity(vpn) & 0x1;
            vm->cache_fetch[index].total_fetch++;
            vm->cache_fetch[index].icache_hits++;
#endif
            *value = *(const uint32_t *) (seq->base + ofs);
            vm->seq_fetch_next_pc =
                ((ofs + sizeof(uint32_t)) < ICACHE_BLOCKS_SIZE) ? (addr + 4)
                                                                : 0xFFFFFFFF;
            return;
        }
    }

    uint32_t idx = (addr >> ICACHE_OFFSET_BITS) & ICACHE_INDEX_MASK;
    uint32_t tag = addr >> (ICACHE_OFFSET_BITS + ICACHE_INDEX_BITS);
    icache_block_t *blk = &vm->icache.block[idx];
    uint32_t vpn = addr >> RV_PAGE_SHIFT;
    uint32_t index = __builtin_parity(vpn) & 0x1;

#ifdef MMU_CACHE_STATS
    vm->cache_fetch[index].total_fetch++;
#endif

    /* I-cache lookup */
    if (likely(blk->valid && blk->tag == tag)) {
        /* I-cache hit */
#ifdef MMU_CACHE_STATS
        vm->cache_fetch[index].icache_hits++;
#endif
        *value = *(const uint32_t *) (blk->base + (addr & ICACHE_BLOCK_MASK));
        vm->seq_fetch_block = blk;
        vm->seq_fetch_next_pc = (((addr & ICACHE_BLOCK_MASK) +
                                  sizeof(uint32_t)) < ICACHE_BLOCKS_SIZE)
                                    ? (addr + 4)
                                    : 0xFFFFFFFF;
        return;
    }
    /* I-cache miss */
    else {
#ifdef MMU_CACHE_STATS
        vm->cache_fetch[index].icache_misses++;
#endif
    }

    /* I-cache miss, 2-entry TLB lookup */
    if (unlikely(vpn != vm->cache_fetch[index].n_pages)) {
        /* TLB miss */
#ifdef MMU_CACHE_STATS
        vm->cache_fetch[index].tlb_misses++;
#endif
        mmu_translate(vm, &addr, (1 << 3), (1 << 6), false, RV_EXC_FETCH_FAULT,
                      RV_EXC_FETCH_PFAULT);
        if (vm->error)
            return;
        uint32_t *page_addr;
        vm->mem_fetch(vm, addr >> RV_PAGE_SHIFT, &page_addr);
        if (vm->error)
            return;
        vm->cache_fetch[index].n_pages = vpn;
        vm->cache_fetch[index].page_addr = page_addr;
    }
    /* TLB hit */
    else {
#ifdef MMU_CACHE_STATS
        vm->cache_fetch[index].tlb_hits++;
#endif
    }

    /* fill into the I-cache */
    uint32_t block_off = (addr & RV_PAGE_MASK) & ~ICACHE_BLOCK_MASK;
    blk->base = (const uint8_t *) vm->cache_fetch[index].page_addr + block_off;
    blk->tag = tag;
    blk->valid = true;
    *value = *(const uint32_t *) (blk->base + (addr & ICACHE_BLOCK_MASK));
    vm->seq_fetch_block = blk;
    vm->seq_fetch_next_pc =
        (((addr & ICACHE_BLOCK_MASK) + sizeof(uint32_t)) < ICACHE_BLOCKS_SIZE)
            ? (addr + 4)
            : 0xFFFFFFFF;
}

static inline uint32_t *ram_cache_lookup(hart_t *vm,
                                         uint32_t phys_addr,
                                         bool is_store)
{
    uint32_t page = phys_addr >> RV_PAGE_SHIFT;
    uint32_t *page_base;

    if (unlikely(!vm->ram_base || phys_addr >= vm->ram_size))
        return NULL;

    if (is_store) {
        if (likely(vm->ram_store_last_page == page))
            return vm->ram_store_last_ptr;
    } else {
        if (likely(vm->ram_load_last_page == page))
            return vm->ram_load_last_ptr;
    }

    page_base = vm->ram_base + (page << (RV_PAGE_SHIFT - 2));

    if (is_store) {
        vm->ram_store_last_page = page;
        vm->ram_store_last_ptr = page_base;
        return vm->ram_store_last_ptr;
    }

    vm->ram_load_last_page = page;
    vm->ram_load_last_ptr = page_base;
    return vm->ram_load_last_ptr;
}

static inline void ram_read_fast(hart_t *vm,
                                 uint32_t *page_ptr,
                                 uint32_t phys_addr,
                                 uint8_t width,
                                 uint32_t *value)
{
    uint32_t offset = phys_addr & RV_PAGE_MASK;
    uint32_t shift = (offset & 0x3) * 8;
    uint32_t *cell = &page_ptr[offset >> 2];

    switch (width) {
    case RV_MEM_LW:
        if (unlikely(offset & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
        *value = *cell;
        return;
    case RV_MEM_LHU:
        if (unlikely(offset & 0x1)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
        *value = (uint16_t) (*cell >> shift);
        return;
    case RV_MEM_LH:
        if (unlikely(offset & 0x1)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
        *value = (uint32_t) (int32_t) (int16_t) (*cell >> shift);
        return;
    case RV_MEM_LBU:
        *value = (uint8_t) (*cell >> shift);
        return;
    case RV_MEM_LB:
        *value = (uint32_t) (int32_t) (int8_t) (*cell >> shift);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

static inline void ram_write_fast(hart_t *vm,
                                  uint32_t *page_ptr,
                                  uint32_t phys_addr,
                                  uint8_t width,
                                  uint32_t value)
{
    uint32_t offset = phys_addr & RV_PAGE_MASK;
    uint32_t shift = (offset & 0x3) * 8;
    uint32_t *cell = &page_ptr[offset >> 2];

    switch (width) {
    case RV_MEM_SW:
        if (unlikely(offset & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        *cell = value;
        return;
    case RV_MEM_SH:
        if (unlikely(offset & 0x1)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        *cell = (*cell & ~(MASK(16) << shift)) | ((value & MASK(16)) << shift);
        return;
    case RV_MEM_SB:
        *cell = (*cell & ~(MASK(8) << shift)) | ((value & MASK(8)) << shift);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

static void mmu_load(hart_t *vm,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value,
                     bool reserved)
{
    vm->exc_val = addr;
    uint32_t vpn = addr >> RV_PAGE_SHIFT;
    uint32_t phys_addr;
    if (likely(vm->cache_load_last_vpn == vpn)) {
        phys_addr = (vm->cache_load_last_phys_ppn << RV_PAGE_SHIFT) |
                    (addr & MASK(RV_PAGE_SHIFT));
    } else {
        /* 8-set × 2-way set-associative cache: use 3-bit parity hash */
        uint32_t set_idx = (__builtin_parity(vpn & 0xAAAAAAAA) << 2) |
                           (__builtin_parity(vpn & 0x55555555) << 1) |
                           __builtin_parity(vpn & 0xCCCCCCCC);
        mmu_cache_set_t *set = &vm->cache_load[set_idx];
        int hit_way = -1;

        /* Check both ways in the set */
        for (int way = 0; way < 2; way++) {
            if (likely(set->ways[way].n_pages == vpn)) {
                hit_way = way;
                break;
            }
        }

        if (likely(hit_way >= 0)) {
            /* Cache hit: reconstruct physical address from cached PPN */
#ifdef MMU_CACHE_STATS
            set->ways[hit_way].hits++;
#endif
            phys_addr = (set->ways[hit_way].phys_ppn << RV_PAGE_SHIFT) |
                        (addr & MASK(RV_PAGE_SHIFT));
            /* Update LRU: mark the other way as replacement candidate */
            set->lru = 1 - hit_way;
        } else {
            /* Cache miss: do full translation */
            int victim_way = set->lru; /* Use LRU bit to select victim */
#ifdef MMU_CACHE_STATS
            set->ways[victim_way].misses++;
#endif
            phys_addr = addr;
            mmu_translate(vm, &phys_addr,
                          (1 << 1) | (vm->sstatus_mxr ? (1 << 3) : 0), (1 << 6),
                          vm->sstatus_sum && vm->s_mode, RV_EXC_LOAD_FAULT,
                          RV_EXC_LOAD_PFAULT);
            if (vm->error)
                return;
            /* Replace victim way with new translation */
            set->ways[victim_way].n_pages = vpn;
            set->ways[victim_way].phys_ppn = phys_addr >> RV_PAGE_SHIFT;
            /* Update LRU: mark the other way for next eviction */
            set->lru = 1 - victim_way;
        }

        vm->cache_load_last_vpn = vpn;
        vm->cache_load_last_phys_ppn = phys_addr >> RV_PAGE_SHIFT;
    }

    uint32_t *page_ptr = ram_cache_lookup(vm, phys_addr, false);
    if (likely(page_ptr != NULL)) {
        ram_read_fast(vm, page_ptr, phys_addr, width, value);
        if (vm->error)
            return;
    } else {
        vm->mem_load(vm, phys_addr, width, value);
        if (vm->error)
            return;
    }

    if (unlikely(reserved))
        vm->lr_reservation = phys_addr | 1;
}

static bool mmu_store(hart_t *vm,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value,
                      bool cond)
{
    vm->exc_val = addr;
    uint32_t vpn = addr >> RV_PAGE_SHIFT;
    uint32_t phys_addr;
    if (likely(vm->cache_store_last_vpn == vpn)) {
        phys_addr = (vm->cache_store_last_phys_ppn << RV_PAGE_SHIFT) |
                    (addr & MASK(RV_PAGE_SHIFT));
    } else {
        /* 8-set × 2-way set-associative cache: use 3-bit parity hash */
        uint32_t set_idx = (__builtin_parity(vpn & 0xAAAAAAAA) << 2) |
                           (__builtin_parity(vpn & 0x55555555) << 1) |
                           __builtin_parity(vpn & 0xCCCCCCCC);
        mmu_cache_set_t *set = &vm->cache_store[set_idx];
        int hit_way = -1;

        /* Check both ways in the set */
        for (int way = 0; way < 2; way++) {
            if (likely(set->ways[way].n_pages == vpn)) {
                hit_way = way;
                break;
            }
        }

        if (likely(hit_way >= 0)) {
            /* Cache hit: reconstruct physical address from cached PPN */
#ifdef MMU_CACHE_STATS
            set->ways[hit_way].hits++;
#endif
            phys_addr = (set->ways[hit_way].phys_ppn << RV_PAGE_SHIFT) |
                        (addr & MASK(RV_PAGE_SHIFT));
            /* Update LRU: mark the other way as replacement candidate */
            set->lru = 1 - hit_way;
        } else {
            /* Cache miss: do full translation */
            int victim_way = set->lru; /* Use LRU bit to select victim */
#ifdef MMU_CACHE_STATS
            set->ways[victim_way].misses++;
#endif
            phys_addr = addr;
            mmu_translate(vm, &phys_addr, (1 << 2), (1 << 6) | (1 << 7),
                          vm->sstatus_sum && vm->s_mode, RV_EXC_STORE_FAULT,
                          RV_EXC_STORE_PFAULT);
            if (vm->error)
                return false;
            /* Replace victim way with new translation */
            set->ways[victim_way].n_pages = vpn;
            set->ways[victim_way].phys_ppn = phys_addr >> RV_PAGE_SHIFT;
            /* Update LRU: mark the other way for next eviction */
            set->lru = 1 - victim_way;
        }

        vm->cache_store_last_vpn = vpn;
        vm->cache_store_last_phys_ppn = phys_addr >> RV_PAGE_SHIFT;
    }

    if (unlikely(cond)) {
        if ((vm->lr_reservation != (phys_addr | 1)))
            return false;
    }

    for (uint32_t i = 0; i < vm->vm->n_hart; i++) {
        if (unlikely(vm->vm->hart[i]->lr_reservation & 1) &&
            (vm->vm->hart[i]->lr_reservation & ~3) == (phys_addr & ~3))
            vm->vm->hart[i]->lr_reservation = 0;
    }
    uint32_t *page_ptr = ram_cache_lookup(vm, phys_addr, true);
    if (likely(page_ptr != NULL)) {
        ram_write_fast(vm, page_ptr, phys_addr, width, value);
        return true;
    }

    vm->mem_store(vm, phys_addr, width, value);
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

    /* After the booting process is complete, initrd will be loaded. At this
     * point, the sytstem will switch to U mode for the first time. Therefore,
     * by checking whether the switch to U mode has already occurred, we can
     * determine if the boot process has been completed.
     */
    if (!boot_complete && !vm->s_mode)
        boot_complete = true;

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
        /* Call the WFI callback if available */
        if (vm->wfi)
            vm->wfi(vm);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}

/* CSR instructions */

static inline void set_dest_idx(hart_t *vm, uint8_t rd, uint32_t x)
{
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
        *value = semu_timer_get(&vm->time);
        return;
    case RV_CSR_TIMEH:
        *value = semu_timer_get(&vm->time) >> 32;
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
    case RV_CSR_SSTATUS: {
        bool old_sum = vm->sstatus_sum;
        bool old_mxr = vm->sstatus_mxr;
        vm->sstatus_sie = (value & (1 << (1))) != 0;
        vm->sstatus_spie = (value & (1 << (5))) != 0;
        vm->sstatus_spp = (value & (1 << (8))) != 0;
        vm->sstatus_sum = (value & (1 << (18))) != 0;
        vm->sstatus_mxr = (value & (1 << (19))) != 0;
        /* Invalidate load/store TLB if SUM or MXR changed */
        if (vm->sstatus_sum != old_sum || vm->sstatus_mxr != old_mxr)
            mmu_invalidate(vm);
        break;
    }
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

static void op_csr_rw(hart_t *vm, uint8_t rd, uint16_t csr, uint32_t wvalue)
{
    if (rd) {
        uint32_t value;
        csr_read(vm, csr, &value);
        if (unlikely(vm->error))
            return;
        set_dest_idx(vm, rd, value);
    }
    csr_write(vm, csr, wvalue);
}

static void op_csr_cs(hart_t *vm,
                      uint8_t rd,
                      uint8_t rs1,
                      uint16_t csr,
                      uint32_t setmask,
                      uint32_t clearmask)
{
    uint32_t value;
    csr_read(vm, csr, &value);
    if (unlikely(vm->error))
        return;
    set_dest_idx(vm, rd, value);
    if (rs1)
        csr_write(vm, csr, (value & ~clearmask) | setmask);
}

static void op_system(hart_t *vm, const decoded_insn_t *decoded)
{
    switch (decoded_funct3(decoded)) {
    /* CSR */
    case 0b001: /* CSRRW */
        op_csr_rw(vm, decoded_rd(decoded), decoded->imm,
                  vm->x_regs[decoded_rs1(decoded)]);
        break;
    case 0b101: /* CSRRWI */
        op_csr_rw(vm, decoded_rd(decoded), decoded->imm, decoded_rs1(decoded));
        break;
    case 0b010: /* CSRRS */
        op_csr_cs(vm, decoded_rd(decoded), decoded_rs1(decoded), decoded->imm,
                  vm->x_regs[decoded_rs1(decoded)], 0);
        break;
    case 0b110: /* CSRRSI */
        op_csr_cs(vm, decoded_rd(decoded), decoded_rs1(decoded), decoded->imm,
                  decoded_rs1(decoded), 0);
        break;
    case 0b011: /* CSRRC */
        op_csr_cs(vm, decoded_rd(decoded), decoded_rs1(decoded), decoded->imm,
                  0, vm->x_regs[decoded_rs1(decoded)]);
        break;
    case 0b111: /* CSRRCI */
        op_csr_cs(vm, decoded_rd(decoded), decoded_rs1(decoded), decoded->imm,
                  0, decoded_rs1(decoded));
        break;

    /* privileged instruction */
    case 0b000: /* SYS_PRIV */
        op_privileged(
            vm, (decoded_funct7(decoded) << 25) | (decoded_rs2(decoded) << 20) |
                    (decoded_rs1(decoded) << 15) |
                    (decoded_funct3(decoded) << 12) |
                    (decoded_rd(decoded) << 7) | decoded_opcode(decoded));
        break;

    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

/* Unprivileged instructions */

static uint32_t op_mul(uint8_t funct3, uint32_t a, uint32_t b)
{
    /* TODO: Test ifunc7 zeros */
    switch (funct3) {
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

static uint32_t op_rv32i(uint8_t funct3,
                         bool neg,
                         bool is_reg,
                         uint32_t a,
                         uint32_t b)
{
    /* TODO: Test ifunc7 zeros */
    switch (funct3) {
    case 0b000: /* IFUNC_ADD */
        return a + ((is_reg && neg) ? -b : b);
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
    case 0b101:                                                  /* IFUNC_SRL */
        return neg ? (uint32_t) (((int32_t) a) >> (b & MASK(5))) /* SRA */
                   : a >> (b & MASK(5)) /* SRL */;
    }
    __builtin_unreachable();
}

static bool op_jmp(hart_t *vm, uint8_t funct3, uint32_t a, uint32_t b)
{
    switch (funct3) {
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

static void op_jump_link(hart_t *vm, uint8_t rd, uint32_t addr)
{
    if (unlikely(addr & 0b11)) {
        vm_set_exception(vm, RV_EXC_PC_MISALIGN, addr);
    } else {
        set_dest_idx(vm, rd, vm->pc);
        vm->pc = addr;
    }
}

#define AMO_OP(STORED_EXPR)                                   \
    do {                                                      \
        value2 = vm->x_regs[decoded_rs2(decoded)];            \
        mmu_load(vm, addr, RV_MEM_LW, &value, false);         \
        if (vm->error)                                        \
            return;                                           \
        set_dest_idx(vm, decoded_rd(decoded), value);         \
        mmu_store(vm, addr, RV_MEM_SW, (STORED_EXPR), false); \
        if (vm->error)                                        \
            return;                                           \
    } while (0)

static void op_amo(hart_t *vm, const decoded_insn_t *decoded)
{
    if (unlikely(decoded_funct3(decoded) != 0b010 /* amo.w */))
        return vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
    uint32_t addr = vm->x_regs[decoded_rs1(decoded)];
    uint32_t value, value2;
    switch (decoded_funct5(decoded)) {
    case 0b00010: /* AMO_LR */
        if (addr & 0b11)
            return vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, addr);
        if (decoded_rs2(decoded))
            return vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        mmu_load(vm, addr, RV_MEM_LW, &value, true);
        if (vm->error)
            return;
        set_dest_idx(vm, decoded_rd(decoded), value);
        break;
    case 0b00011: /* AMO_SC */
        if (addr & 0b11)
            return vm_set_exception(vm, RV_EXC_STORE_MISALIGN, addr);
        bool ok = mmu_store(vm, addr, RV_MEM_SW,
                            vm->x_regs[decoded_rs2(decoded)], true);
        if (vm->error)
            return;
        set_dest_idx(vm, decoded_rd(decoded), ok ? 0 : 1);
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
    vm->ram_load_last_page = 0xFFFFFFFF;
    vm->ram_store_last_page = 0xFFFFFFFF;
}

#define PRIV(x) ((emu_state_t *) x->priv)
static inline void vm_handle_pending_interrupt(hart_t *vm)
{
    if ((vm->sstatus_sie || !vm->s_mode) && (vm->sip & vm->sie)) {
        uint32_t applicable = (vm->sip & vm->sie);
        uint8_t idx = ilog2(applicable);
        if (idx == 1) {
            emu_state_t *data = PRIV(vm);
            data->sswi.ssip[vm->mhartid] = 0;
        }
        vm->exc_cause = (1U << 31) | idx;
        vm->stval = 0;
        hart_trap(vm);
    }
}

static inline bool vm_execute_insn(hart_t *vm, uint32_t insn)
{
    uint32_t value;
    uint8_t opcode;
    uint32_t *x_regs = vm->x_regs;

    opcode = insn & MASK(7);

    switch (opcode) {
    case RV32_OP_IMM: {
        uint8_t funct3 = decode_func3(insn);
        uint8_t rd = decode_rd(insn);
        uint32_t rs1 = x_regs[decode_rs1(insn)];
        bool neg = (insn & (1 << 30)) != 0;

        set_dest_idx(vm, rd, op_rv32i(funct3, neg, false, rs1, decode_i(insn)));
        return true;
    }
    case RV32_OP: {
        uint8_t funct3 = decode_func3(insn);
        uint8_t rd = decode_rd(insn);
        uint32_t rs1 = x_regs[decode_rs1(insn)];
        uint32_t rs2 = x_regs[decode_rs2(insn)];
        bool neg = (insn & (1 << 30)) != 0;

        if (!(insn & (1 << 25)))
            set_dest_idx(vm, rd, op_rv32i(funct3, neg, true, rs1, rs2));
        else
            set_dest_idx(vm, rd, op_mul(funct3, rs1, rs2));
        return true;
    }
    case RV32_LUI:
        set_dest_idx(vm, decode_rd(insn), decode_u(insn));
        return true;
    case RV32_AUIPC:
        set_dest_idx(vm, decode_rd(insn), decode_u(insn) + vm->current_pc);
        return true;
    case RV32_JAL:
        op_jump_link(vm, decode_rd(insn), decode_j(insn) + vm->current_pc);
        return true;
    case RV32_JALR:
        op_jump_link(vm, decode_rd(insn),
                     (decode_i(insn) + x_regs[decode_rs1(insn)]) & ~1U);
        return true;
    case RV32_BRANCH: {
        uint8_t funct3 = decode_func3(insn);
        uint32_t rs1 = x_regs[decode_rs1(insn)];
        uint32_t rs2 = x_regs[decode_rs2(insn)];

        if (op_jmp(vm, funct3, rs1, rs2))
            do_jump(vm, decode_b(insn) + vm->current_pc);
        return true;
    }
    case RV32_LOAD:
        mmu_load(vm, x_regs[decode_rs1(insn)] + decode_i(insn),
                 decode_func3(insn), &value, false);
        if (unlikely(vm->error))
            return false;
        set_dest_idx(vm, decode_rd(insn), value);
        return true;
    case RV32_STORE:
        mmu_store(vm, x_regs[decode_rs1(insn)] + decode_s(insn),
                  decode_func3(insn), x_regs[decode_rs2(insn)], false);
        if (unlikely(vm->error))
            return false;
        return true;
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
        return false;
    case RV32_AMO: {
        decoded_insn_t decoded;
        decode_insn(&decoded, insn);
        op_amo(vm, &decoded);
        return false;
    }
    case RV32_SYSTEM: {
        decoded_insn_t decoded;
        decode_insn(&decoded, insn);
        op_system(vm, &decoded);
        return false;
    }
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return false;
    }
}

int vm_step_many(hart_t *vm, int steps)
{
    const uint32_t *seq_ptr = NULL;
    uint32_t seq_next_pc = 0xFFFFFFFF;
    uint32_t seq_limit_pc = 0;
    int executed = 0;

    if (vm->hsm_status != SBI_HSM_STATE_STARTED || unlikely(vm->error))
        return 0;

    for (; executed < steps; executed++) {
        uint32_t insn;
        uint32_t pc = vm->pc;

        vm->current_pc = pc;
        vm_handle_pending_interrupt(vm);
        if (unlikely(vm->pc != pc)) {
            seq_ptr = NULL;
            seq_next_pc = 0xFFFFFFFF;
        }
        vm->current_pc = vm->pc;
        if (likely(seq_ptr != NULL && vm->pc == seq_next_pc)) {
            insn = *seq_ptr++;
            seq_next_pc += 4;
            if (seq_next_pc >= seq_limit_pc) {
                seq_ptr = NULL;
                seq_next_pc = 0xFFFFFFFF;
            }
        } else {
            mmu_fetch(vm, vm->pc, &insn);
            if (unlikely(vm->error))
                return executed;

            if (likely(vm->seq_fetch_block != NULL)) {
                uint32_t block_pc = vm->pc & ~ICACHE_BLOCK_MASK;
                uint32_t offset_words = (vm->pc & ICACHE_BLOCK_MASK) >> 2;

                seq_ptr = ((const uint32_t *) vm->seq_fetch_block->base) +
                          offset_words + 1;
                seq_next_pc = vm->pc + 4;
                seq_limit_pc = block_pc + ICACHE_BLOCKS_SIZE;
                if (seq_next_pc >= seq_limit_pc) {
                    seq_ptr = NULL;
                    seq_next_pc = 0xFFFFFFFF;
                }
            }
        }

        bool keep_linear;

        vm->pc += 4;
        keep_linear = vm_execute_insn(vm, insn);
        if (unlikely(vm->error))
            return executed + 1;
        vm->instret++;

        if (unlikely(!keep_linear || vm->pc != vm->current_pc + 4)) {
            seq_ptr = NULL;
            seq_next_pc = 0xFFFFFFFF;
        }
    }

    return executed;
}

void vm_step(hart_t *vm)
{
    if (vm->hsm_status != SBI_HSM_STATE_STARTED)
        return;

    if (unlikely(vm->error))
        return;

    (void) vm_step_many(vm, 1);
}
