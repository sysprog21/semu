#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "utils.h"

/* ERR_EXCEPTION indicates that the instruction has raised one of the
 * exceptions defined in the specification. If this flag is set, the
 * additional fields "exc_cause" and "exc_val" must also be set to values
 * as defined in the specification.
 *
 * A common approach to handling exceptions includes:
 *  - Environment call: since the program counter (pc) has already been
 *    advanced, environment calls can be handled by simply clearing the
 *    error after completing the operation.
 *  - Faults: when handling faults, it is important to set
 *    vm->pc = vm->current_pc so that the instruction can be retried.
 *  - Traps: exceptions can be delegated to the emulated code in the form
 *    of S-mode traps by invoking "hart_trap()". This function takes care
 *    of clearing the error.
 *
 * ERR_USER is not set by any "vm_*()" function. It is reserved for user
 * callbacks to halt instruction execution and trigger the return of
 * "vm_step()". If the flag is not set during a fetch operation, the pc
 * will be advanced accordingly.
 */
typedef enum {
    ERR_NONE,
    ERR_EXCEPTION, /**< RISC-V exception was raised (see additional fields) */
    ERR_USER,      /**< user-specific error */
} vm_error_t;

/* Instruction fetch cache: stores host memory pointers for direct access */
typedef struct {
    uint32_t n_pages;
    uint32_t *page_addr;
#ifdef MMU_CACHE_STATS
    uint64_t total_fetch;
    uint64_t tlb_hits, tlb_misses;
    uint64_t icache_hits, icache_misses;
#endif
} mmu_fetch_cache_t;

/* Load/store cache: stores physical page numbers (not pointers) */
typedef struct {
    uint32_t n_pages;
    uint32_t phys_ppn;         /* Physical page number */
    uintptr_t data_minus_addr; /* host_ptr - guest_addr; 0 if not RAM */
#ifdef MMU_CACHE_STATS
    uint64_t hits;
    uint64_t misses;
#endif
} mmu_addr_cache_t;

/* Set-associative cache structure for load operations */
typedef struct {
    mmu_addr_cache_t ways[2]; /* 2-way associative */
    uint8_t lru;              /* LRU bit: 0 or 1 (which way to replace) */
} mmu_cache_set_t;

/* To use the emulator, start by initializing a hart_t object with zero values,
 * invoke vm_init(), and set the required environment-supplied callbacks. You
 * may also set other necessary fields such as argument registers and s_mode,
 * ensuring that all field restrictions are met to avoid undefined behavior.
 *
 * Once the emulator is set up, execute the emulation loop by calling
 * "vm_step()" repeatedly. Each call attempts to execute a single instruction.
 *
 * If the execution completes successfully, the "vm->error" field will be set
 * to ERR_NONE. However, if an error occurs during execution, the emulator will
 * halt and the "vm->error" field will provide information about the error. It
 * is important to handle the emulation error before calling "vm_step()" again;
 * otherwise, it will not execute any instructions. The possible errors are
 * described above for reference.
 */
typedef struct __hart_internal hart_t;
typedef struct __vm_internel vm_t;

/* ICACHE_BLOCKS_SIZE: Size of one instruction-cache block (line).
 * ICACHE_BLOCKS: Number of blocks (lines) in the instruction cache.
 *
 * The cache address is decomposed into [ tag | index | offset ] fields:
 *   - block-offset bits = log2(ICACHE_BLOCKS_SIZE)
 *   - index bits        = log2(ICACHE_BLOCKS)
 */
#define ICACHE_BLOCKS_SIZE 256
#define ICACHE_BLOCKS 256
#define ICACHE_OFFSET_BITS 8
#define ICACHE_INDEX_BITS 8

/* For power-of-two sizes, (size - 1) sets all low bits to 1,
 * allowing fast extraction of an address.
 */
#define ICACHE_INDEX_MASK (ICACHE_BLOCKS - 1)
#define ICACHE_BLOCK_MASK (ICACHE_BLOCKS_SIZE - 1)
#define RV_PAGE_MASK (RV_PAGE_SIZE - 1)

typedef struct {
    uint32_t imm;
    uint32_t fields;
} decoded_insn_t;

typedef struct {
    uint32_t tag;
    uint32_t epoch;
    const uint8_t *base;
    bool valid;
} icache_block_t;

typedef struct {
    icache_block_t block[ICACHE_BLOCKS];
} icache_t;

struct __hart_internal {
    /* Hot path: accessed every instruction (cache lines 0-4) */
    uint32_t x_regs[32];
    uint32_t pc;
    uint32_t current_pc;
    uint64_t instret;
    vm_error_t error;
    uint32_t exc_cause, exc_val;
    uint32_t lr_reservation;

    /* Load/store TLB last-entry fast path */
    uint32_t cache_load_last_vpn;
    uint32_t cache_load_last_phys_ppn;
    uintptr_t cache_load_last_data_minus_addr;
    uint32_t cache_store_last_vpn;
    uint32_t cache_store_last_phys_ppn;
    uintptr_t cache_store_last_data_minus_addr;

    /* Instruction fetch sequence state */
    icache_block_t *seq_fetch_block;
    uint32_t seq_fetch_next_pc;
    uint32_t icache_epoch;

    /* Direct RAM access */
    uint32_t *ram_base;
    uint32_t ram_size;
    uint32_t ram_load_last_page;
    uint32_t *ram_load_last_ptr;
    uint32_t ram_store_last_page;
    uint32_t *ram_store_last_ptr;

    /* Warm path: interrupt check fields */
    bool sstatus_sie;
    bool s_mode;
    uint32_t sie;
    uint32_t sip;

    semu_timer_t time;

    /* Supervisor state */
    bool sstatus_spp;
    bool sstatus_spie;
    uint32_t sepc;
    bool in_wfi;
    uint32_t scause;
    uint32_t stval;
    bool sstatus_mxr;
    bool sstatus_sum;
    uint32_t stvec_addr;
    bool stvec_vectored;
    uint32_t sscratch;
    uint32_t scounteren;
    uint32_t satp;
    uint32_t *page_table;

    /* Machine state */
    uint32_t mhartid;

    void *priv;

    void (*wfi)(hart_t *vm);

    void (*mem_fetch)(hart_t *vm, uint32_t n_pages, uint32_t **page_addr);
    void (*mem_load)(hart_t *vm, uint32_t addr, uint8_t width, uint32_t *value);
    void (*mem_store)(hart_t *vm, uint32_t addr, uint8_t width, uint32_t value);
    uint32_t *(*mem_page_table)(const hart_t *vm, uint32_t ppn);

    vm_t *vm;
    int32_t hsm_status;
    bool hsm_resume_is_ret;
    int32_t hsm_resume_pc;
    int32_t hsm_resume_opaque;

    /* Cold: set-associative caches */
    mmu_fetch_cache_t cache_fetch[16];
    mmu_cache_set_t cache_load[32];
    mmu_cache_set_t cache_store[32];
    icache_t icache;
};

struct __vm_internel {
    uint32_t n_hart;
    hart_t **hart;
};

void vm_init(hart_t *vm);

/* Emulate the next instruction. This is a no-op if the error is already set. */
void vm_step(hart_t *vm);

/* Emulate up to "steps" instructions without per-instruction interrupt checks.
 * Returns the number of attempted instructions, including one that raised an
 * exception or fatal error.
 */
int vm_step_many(hart_t *vm, int steps);

/* Raise a RISC-V exception. This is equivalent to setting vm->error to
 * ERR_EXCEPTION and setting the accompanying fields. It is provided as
 * a function for convenience and to prevent mistakes such as forgetting to
 * set a field.
 */
void vm_set_exception(hart_t *vm, uint32_t cause, uint32_t val);

/* Delegate the currently set exception to S-mode as a trap. This function does
 * not check if vm->error is EXC_EXCEPTION; it assumes that "exc_cause" and
 * "exc_val" are correctly set. It sets vm->error to ERR_NONE.
 */
void hart_trap(hart_t *vm);

/* Return a readable description for a RISC-V exception cause */
void vm_error_report(const hart_t *vm);

/* Invalidate all MMU translation caches (fetch, load, store) */
void mmu_invalidate(hart_t *vm);

/* Invalidate MMU caches for a specific virtual address range */
void mmu_invalidate_range(hart_t *vm, uint32_t start_addr, uint32_t size);

/* Invalidate instruction cache (FENCE.I) */
void vm_fence_i(hart_t *vm);
