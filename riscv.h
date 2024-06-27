#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 *    of S-mode traps by invoking "vm_trap()". This function takes care
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

typedef struct {
    uint32_t n_pages;
    uint32_t *page_addr;
} mmu_cache_t;

#define TLB_SIZE 16
typedef struct {
    uint32_t vpn;
    uint32_t *pte;
    bool valid;
    bool dirty;
} tlb_entry_t;

struct node_t {
    tlb_entry_t entry;
    struct node_t *prev;
    struct node_t *next;
};

typedef struct {
    struct node_t *head;
    struct node_t *tail;
    uint32_t size;
} tlb_list_t;

/* To use the emulator, start by initializing a vm_t object with zero values,
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
typedef struct __vm_internal vm_t;

struct __vm_internal {
    uint32_t x_regs[32];

    /* LR reservation virtual address. last bit is 1 if valid */
    uint32_t lr_reservation;

    /* Assumed to contain an aligned address at all times */
    uint32_t pc;

    /* Address of last instruction that began execution */
    uint32_t current_pc;

    /* 'instructions executed' 64-bit counter serves as a real-time clock,
     * instruction-retired counter, and cycle counter. It is currently
     * utilized in these capacities and should not be modified between logical
     * resets.
     */
    uint64_t insn_count;

    /* Instruction execution state must be set to "NONE" for instruction
     * execution to continue. If the state is not "NONE," the vm_step()
     * function will exit.
     */
    vm_error_t error;

    /* If the error value is ERR_EXCEPTION, the specified values will be used
     * for the scause and stval registers if they are turned into a trap.
     * Refer to the RISC-V specification for the meaning of these values.
     */
    uint32_t exc_cause, exc_val;

    mmu_cache_t cache_fetch;
    tlb_list_t tlb_list;

    /* Supervisor state */
    bool s_mode;
    bool sstatus_spp; /**< state saved at trap */
    bool sstatus_spie;
    uint32_t sepc;
    uint32_t scause;
    uint32_t stval;
    bool sstatus_mxr; /**< alter MMU access rules */
    bool sstatus_sum;
    bool sstatus_sie; /**< interrupt state */
    uint32_t sie;
    uint32_t sip;
    uint32_t stvec_addr; /**< trap config */
    bool stvec_vectored;
    uint32_t sscratch; /**< misc */
    uint32_t scounteren;
    uint32_t satp; /**< MMU */
    uint32_t *page_table;

    void *priv; /**< environment supplied */

    /* Memory access sets the vm->error to indicate failure. On successful
     * access, it reads or writes the specified "value".
     */
    void (*mem_fetch)(vm_t *vm, uint32_t n_pages, uint32_t **page_addr);
    void (*mem_load)(vm_t *vm, uint32_t addr, uint8_t width, uint32_t *value);
    void (*mem_store)(vm_t *vm, uint32_t addr, uint8_t width, uint32_t value);

    /* Pre-validate whether the required page number can accommodate a page
     * table. If it is not a valid page, it returns NULL. The function returns
     * a uint32_t * to the page if valid.
     */
    uint32_t *(*mem_page_table)(const vm_t *vm, uint32_t ppn);
};

void vm_init(vm_t *vm);

/* Emulate the next instruction. This is a no-op if the error is already set. */
void vm_step(vm_t *vm);

/* Raise a RISC-V exception. This is equivalent to setting vm->error to
 * ERR_EXCEPTION and setting the accompanying fields. It is provided as
 * a function for convenience and to prevent mistakes such as forgetting to
 * set a field.
 */
void vm_set_exception(vm_t *vm, uint32_t cause, uint32_t val);

/* Delegate the currently set exception to S-mode as a trap. This function does
 * not check if vm->error is EXC_EXCEPTION; it assumes that "exc_cause" and
 * "exc_val" are correctly set. It sets vm->error to ERR_NONE.
 */
void vm_trap(vm_t *vm);

/* Return a readable description for a RISC-V exception cause */
void vm_error_report(const vm_t *vm);
