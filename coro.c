/* Lightweight coroutine for multi-hart execution */

#include "coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Platform detection */

#if !defined(CORO_USE_UCONTEXT) && !defined(CORO_USE_ASM)
#if __GNUC__ >= 3
#if defined(__x86_64__) || defined(__aarch64__)
#define CORO_USE_ASM
#else
#define CORO_USE_UCONTEXT
#endif
#else
#define CORO_USE_UCONTEXT
#endif
#endif

/* Coroutine state */

typedef enum {
    CORO_STATE_SUSPENDED,
    CORO_STATE_RUNNING,
    CORO_STATE_DEAD
} coro_state_t;

/* Platform-specific context buffer and assembly implementation */

#ifdef CORO_USE_ASM

#if defined(__x86_64__)
/* x86-64 context buffer - stores callee-saved registers */
typedef struct {
    void *rip, *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15;
} coro_ctxbuf_t;

/* Forward declarations for assembly functions */
void _coro_wrap_main(void);
int _coro_switch(coro_ctxbuf_t *from, coro_ctxbuf_t *to);

/* Assembly implementation for x86-64 (macOS and Linux) */
__asm__(
    ".text\n"
#ifdef __MACH__ /* macOS assembler */
    ".globl __coro_wrap_main\n"
    "__coro_wrap_main:\n"
#else /* Linux assembler */
    ".globl _coro_wrap_main\n"
    ".type _coro_wrap_main @function\n"
    ".hidden _coro_wrap_main\n"
    "_coro_wrap_main:\n"
#endif
    "  movq %r13, %rdi\n" /* Load coroutine pointer into first argument */
    "  jmpq *%r12\n"      /* Jump to the coroutine entry point */
#ifndef __MACH__
    ".size _coro_wrap_main, .-_coro_wrap_main\n"
#endif
);

__asm__(
    ".text\n"
#ifdef __MACH__ /* macOS assembler */
    ".globl __coro_switch\n"
    "__coro_switch:\n"
#else /* Linux assembler */
    ".globl _coro_switch\n"
    ".type _coro_switch @function\n"
    ".hidden _coro_switch\n"
    "_coro_switch:\n"
#endif
    /* Save current context (first argument: from) */
    "  leaq 0x3d(%rip), %rax\n" /* Load return address */
    "  movq %rax, (%rdi)\n"     /* Save RIP */
    "  movq %rsp, 8(%rdi)\n"    /* Save RSP */
    "  movq %rbp, 16(%rdi)\n"   /* Save RBP */
    "  movq %rbx, 24(%rdi)\n"   /* Save RBX */
    "  movq %r12, 32(%rdi)\n"   /* Save R12 */
    "  movq %r13, 40(%rdi)\n"   /* Save R13 */
    "  movq %r14, 48(%rdi)\n"   /* Save R14 */
    "  movq %r15, 56(%rdi)\n"   /* Save R15 */
    /* Restore new context (second argument: to) */
    "  movq 56(%rsi), %r15\n" /* Restore R15 */
    "  movq 48(%rsi), %r14\n" /* Restore R14 */
    "  movq 40(%rsi), %r13\n" /* Restore R13 */
    "  movq 32(%rsi), %r12\n" /* Restore R12 */
    "  movq 24(%rsi), %rbx\n" /* Restore RBX */
    "  movq 16(%rsi), %rbp\n" /* Restore RBP */
    "  movq 8(%rsi), %rsp\n"  /* Restore RSP */
    "  jmpq *(%rsi)\n"        /* Jump to saved RIP */
    "  ret\n"
#ifndef __MACH__
    ".size _coro_switch, .-_coro_switch\n"
#endif
);

#elif defined(__aarch64__)

/* ARM64 context buffer - stores callee-saved registers */
typedef struct {
    void *x[12]; /* x19-x30 */
    void *sp;
    void *lr;
    void *d[8]; /* d8-d15 (floating point) */
} coro_ctxbuf_t;

/* Forward declarations for assembly functions */
void _coro_wrap_main(void);
int _coro_switch(coro_ctxbuf_t *from, coro_ctxbuf_t *to);

/* Assembly implementation for ARM64 (macOS and Linux) */
__asm__(
    ".text\n"
#ifdef __APPLE__
    ".globl __coro_switch\n"
    "__coro_switch:\n"
#else
    ".globl _coro_switch\n"
    ".type _coro_switch #function\n"
    ".hidden _coro_switch\n"
    "_coro_switch:\n"
#endif
    /* Save current context (x0 = from) */
    "  mov x10, sp\n"
    "  mov x11, x30\n"
    "  stp x19, x20, [x0, #(0*16)]\n"
    "  stp x21, x22, [x0, #(1*16)]\n"
    "  stp d8, d9, [x0, #(7*16)]\n"
    "  stp x23, x24, [x0, #(2*16)]\n"
    "  stp d10, d11, [x0, #(8*16)]\n"
    "  stp x25, x26, [x0, #(3*16)]\n"
    "  stp d12, d13, [x0, #(9*16)]\n"
    "  stp x27, x28, [x0, #(4*16)]\n"
    "  stp d14, d15, [x0, #(10*16)]\n"
    "  stp x29, x30, [x0, #(5*16)]\n"
    "  stp x10, x11, [x0, #(6*16)]\n"
    /* Restore new context (x1 = to) */
    "  ldp x19, x20, [x1, #(0*16)]\n"
    "  ldp x21, x22, [x1, #(1*16)]\n"
    "  ldp d8, d9, [x1, #(7*16)]\n"
    "  ldp x23, x24, [x1, #(2*16)]\n"
    "  ldp d10, d11, [x1, #(8*16)]\n"
    "  ldp x25, x26, [x1, #(3*16)]\n"
    "  ldp d12, d13, [x1, #(9*16)]\n"
    "  ldp x27, x28, [x1, #(4*16)]\n"
    "  ldp d14, d15, [x1, #(10*16)]\n"
    "  ldp x29, x30, [x1, #(5*16)]\n"
    "  ldp x10, x11, [x1, #(6*16)]\n"
    "  mov sp, x10\n"
    "  dmb ish\n" /* Data Memory Barrier - ensure memory ops complete */
    "  isb\n"     /* Instruction Sync Barrier - flush pipeline */
    "  br x11\n"
#ifndef __APPLE__
    ".size _coro_switch, .-_coro_switch\n"
#endif
);

__asm__(
    ".text\n"
#ifdef __APPLE__
    ".globl __coro_wrap_main\n"
    "__coro_wrap_main:\n"
#else
    ".globl _coro_wrap_main\n"
    ".type _coro_wrap_main #function\n"
    ".hidden _coro_wrap_main\n"
    "_coro_wrap_main:\n"
#endif
    "  mov x0, x19\n"  /* Load coroutine pointer into first argument */
    "  mov x30, x21\n" /* Set return address */
    "  br x20\n"       /* Branch to the coroutine entry point */
#ifndef __APPLE__
    ".size _coro_wrap_main, .-_coro_wrap_main\n"
#endif
);

#else
#error "Unsupported architecture for assembly method"
#endif

#elif defined(CORO_USE_UCONTEXT)

/* ucontext fallback for other platforms */
#include <ucontext.h>

typedef ucontext_t coro_ctxbuf_t;

#else
#error "No coroutine implementation available for this platform"
#endif

/* Internal context structure */

typedef struct {
    coro_ctxbuf_t ctx;      /* Coroutine context */
    coro_ctxbuf_t back_ctx; /* Caller context (to return to) */
} coro_context_t;

/* Internal coroutine structure */

typedef struct {
    void (*func)(void *);    /* Entry point function (user-provided) */
    void *user_data;         /* User data (hart pointer) */
    coro_state_t state;      /* Current state */
    coro_context_t *context; /* Context buffer */
    void *stack_base;        /* Stack base address */
    size_t stack_size;       /* Stack size */
} coro_t;

/* Global state */

static struct {
    coro_t **coroutines;   /* Array of coroutine pointers */
    uint32_t n_hart;       /* Number of harts */
    uint32_t current_hart; /* Currently executing hart ID */
    bool initialized;      /* True if subsystem initialized */
    coro_t *running;       /* Currently running coroutine */
} coro_state = {0};

/* Stack size for each hart coroutine (1MB - increased for complex execution) */
#define CORO_STACK_SIZE (1024 * 1024)

/* Stack canary value for overflow detection */
#define STACK_CANARY_VALUE 0xDEADBEEFCAFEBABEULL

/* Sentinel value for current_hart when no coroutine is running */
#define CORO_HART_ID_IDLE UINT32_MAX

/* Internal helper functions */

/* Thread-local variable for currently running coroutine */
#if defined(__GNUC__) || defined(__clang__)
static __thread coro_t *tls_running_coro = NULL;
#else
static coro_t *tls_running_coro = NULL;
#endif

static inline void coro_clear_running_state(void)
{
    coro_state.current_hart = CORO_HART_ID_IDLE;
    coro_state.running = NULL;
    tls_running_coro = NULL;
}

/* Get pointer to stack canary (placed at bottom of stack buffer) */
static inline uint64_t *coro_get_canary_ptr(coro_t *co)
{
    return (uint64_t *) co->stack_base;
}

/* Check for stack overflow by verifying the canary value in stack buffer */
static inline void coro_check_stack(coro_t *co)
{
    uint64_t *canary_ptr = coro_get_canary_ptr(co);
    if (*canary_ptr != STACK_CANARY_VALUE) {
        fprintf(stderr,
                "FATAL: Stack overflow detected in coroutine! "
                "Expected canary=0x%llx, got=0x%llx at %p\n",
                (unsigned long long) STACK_CANARY_VALUE,
                (unsigned long long) *canary_ptr, (void *) canary_ptr);
        abort();
    }
}

/* Forward declarations */

#ifdef CORO_USE_ASM
static void coro_entry_wrapper(void *arg);
#endif

/* Context switch implementation */

#ifdef CORO_USE_ASM

/* Initialize a new coroutine context */
static void make_context(coro_t *co,
                         coro_ctxbuf_t *ctx,
                         void *stack_base,
                         size_t stack_size)
{
#if defined(__x86_64__)
    /* Reserve 128 bytes for Red Zone (System V AMD64 ABI) */
    stack_size = stack_size - 128;
    /* Ensure 16-byte alignment per ABI requirement */
    size_t stack_top = ((size_t) stack_base + stack_size) & ~15UL;
    void **stack_high_ptr = (void **) (stack_top - sizeof(size_t));
    stack_high_ptr[0] =
        (void *) (0xdeaddeaddeaddead); /* Dummy return address */
    ctx->rip = (void *) (_coro_wrap_main);
    ctx->rsp = (void *) (stack_high_ptr);
    ctx->r12 = (void *) (coro_entry_wrapper); /* Wrapper function pointer */
    ctx->r13 = (void *) (co);                 /* Coroutine pointer */
#elif defined(__aarch64__)
    /* Ensure 16-byte alignment per AAPCS64 requirement */
    size_t stack_top = ((size_t) stack_base + stack_size) & ~15UL;
    ctx->x[0] = (void *) (co); /* Coroutine pointer (x19) */
    ctx->x[1] =
        (void *) (coro_entry_wrapper); /* Wrapper function pointer (x20) */
    ctx->x[2] = (void *) (0xdeaddeaddeaddead); /* Dummy return address (x21) */
    ctx->sp = (void *) (stack_top);
    ctx->lr = (void *) (_coro_wrap_main);
#endif
}

/* Jump into a coroutine */
static void jump_into(coro_t *co)
{
    coro_context_t *context = co->context;
    coro_state.running = co;
    tls_running_coro = co;
    _coro_switch(&context->back_ctx, &context->ctx);
}

/* Jump out of a coroutine */
static void jump_out(coro_t *co)
{
    coro_context_t *context = co->context;
    coro_clear_running_state();
    _coro_switch(&context->ctx, &context->back_ctx);
}

#elif defined(CORO_USE_UCONTEXT)

/* Wrapper for ucontext entry point */
#if defined(_LP64) || defined(__LP64__)
static void wrap_main_ucontext(unsigned int lo, unsigned int hi)
{
    coro_t *co = (coro_t *) (((size_t) lo) | (((size_t) hi) << 32));
    co->func(co->user_data);
    co->state = CORO_STATE_DEAD;
    jump_out(co); /* CRITICAL: Must jump out, not return (uc_link is NULL) */
}
#else
static void wrap_main_ucontext(unsigned int lo)
{
    coro_t *co = (coro_t *) ((size_t) lo);
    co->func(co->user_data);
    co->state = CORO_STATE_DEAD;
    jump_out(co); /* CRITICAL: Must jump out, not return (uc_link is NULL) */
}
#endif

/* Initialize a new coroutine context */
static int make_context(coro_t *co,
                        coro_ctxbuf_t *ctx,
                        void *stack_base,
                        size_t stack_size)
{
    if (getcontext(ctx) != 0) {
        fprintf(stderr, "coro: failed to get ucontext\n");
        return -1;
    }
    ctx->uc_link = NULL;
    ctx->uc_stack.ss_sp = stack_base;
    ctx->uc_stack.ss_size = stack_size;
    unsigned int lo = (unsigned int) ((size_t) co);
#if defined(_LP64) || defined(__LP64__)
    unsigned int hi = (unsigned int) (((size_t) co) >> 32);
    makecontext(ctx, (void (*)(void)) wrap_main_ucontext, 2, lo, hi);
#else
    makecontext(ctx, (void (*)(void)) wrap_main_ucontext, 1, lo);
#endif
    return 0;
}

/* Jump into a coroutine */
static void jump_into(coro_t *co)
{
    coro_context_t *context = co->context;
    coro_state.running = co;
    tls_running_coro = co;
    swapcontext(&context->back_ctx, &context->ctx);
}

/* Jump out of a coroutine */
static void jump_out(coro_t *co)
{
    coro_context_t *context = co->context;
    coro_clear_running_state();
    swapcontext(&context->ctx, &context->back_ctx);
}

#endif

/* Coroutine entry point wrapper (for assembly method) */

#ifdef CORO_USE_ASM
/* This is called by _coro_wrap_main assembly stub */
static void coro_entry_wrapper(void *arg)
{
    coro_t *co = (coro_t *) arg;
    co->func(co->user_data);
    co->state = CORO_STATE_DEAD;
    jump_out(co);
}
#endif

/* Public API implementation */

bool coro_init(uint32_t n_hart)
{
    if (coro_state.initialized) {
        fprintf(stderr, "coro_init: already initialized\n");
        return false;
    }

    if (n_hart == 0 || n_hart > 32) {
        fprintf(stderr, "coro_init: invalid n_hart=%u\n", n_hart);
        return false;
    }

    coro_state.coroutines = calloc(n_hart, sizeof(coro_t *));
    if (!coro_state.coroutines) {
        fprintf(stderr, "coro_init: failed to allocate coroutines array\n");
        return false;
    }

    coro_state.n_hart = n_hart;
    coro_state.current_hart = CORO_HART_ID_IDLE;
    coro_state.initialized = true;
    coro_state.running = NULL;

    return true;
}

void coro_cleanup(void)
{
    if (!coro_state.initialized)
        return;

    for (uint32_t i = 0; i < coro_state.n_hart; i++) {
        if (coro_state.coroutines[i]) {
            coro_t *co = coro_state.coroutines[i];
            if (co->context) {
                free(co->context);
            }
            if (co->stack_base) {
                free(co->stack_base);
            }
            free(co);
            coro_state.coroutines[i] = NULL;
        }
    }

    free(coro_state.coroutines);
    coro_state.coroutines = NULL;
    coro_state.n_hart = 0;
    coro_state.current_hart = CORO_HART_ID_IDLE; /* Reset to idle state */
    coro_state.initialized = false;
    coro_state.running = NULL;
    tls_running_coro = NULL; /* Reset TLS as well */
}

bool coro_create_hart(uint32_t hart_id, void (*func)(void *), void *hart)
{
    if (!coro_state.initialized) {
        fprintf(stderr, "coro_create_hart: not initialized\n");
        return false;
    }

    if (hart_id >= coro_state.n_hart) {
        fprintf(stderr, "coro_create_hart: invalid hart_id=%u\n", hart_id);
        return false;
    }

    if (!func) {
        fprintf(stderr, "coro_create_hart: func is NULL\n");
        return false;
    }

    if (coro_state.coroutines[hart_id]) {
        fprintf(stderr, "coro_create_hart: hart %u already has coroutine\n",
                hart_id);
        return false;
    }

    /* Allocate coroutine structure */
    coro_t *co = calloc(1, sizeof(coro_t));
    if (!co) {
        fprintf(stderr, "coro_create_hart: failed to allocate coroutine\n");
        return false;
    }

    /* Store user function and data */
    co->func = func;
    co->user_data = hart;
    co->state = CORO_STATE_SUSPENDED;

    /* Allocate context */
    co->context = calloc(1, sizeof(coro_context_t));
    if (!co->context) {
        fprintf(stderr, "coro_create_hart: failed to allocate context\n");
        free(co);
        return false;
    }

    /* Allocate stack */
    co->stack_size = CORO_STACK_SIZE;
    co->stack_base = malloc(co->stack_size);
    if (!co->stack_base) {
        fprintf(stderr, "coro_create_hart: failed to allocate stack\n");
        free(co->context);
        free(co);
        return false;
    }

    /* Place canary at bottom of stack buffer (first 8 bytes)
     * Stack grows downward from top, so overflow will hit canary first */
    uint64_t *canary_ptr = coro_get_canary_ptr(co);
    *canary_ptr = STACK_CANARY_VALUE;

    /* Adjust usable stack to skip canary area
     * Stack starts after the canary (bottom + sizeof(uint64_t)) */
    void *usable_stack_base = (uint8_t *) co->stack_base + sizeof(uint64_t);
    size_t usable_stack_size = co->stack_size - sizeof(uint64_t);

    /* Initialize context with adjusted stack bounds */
#ifdef CORO_USE_ASM
    make_context(co, &co->context->ctx, usable_stack_base, usable_stack_size);
#else
    if (make_context(co, &co->context->ctx, usable_stack_base,
                     usable_stack_size) != 0) {
        free(co->stack_base);
        free(co->context);
        free(co);
        return false;
    }
#endif

    coro_state.coroutines[hart_id] = co;
    return true;
}

void coro_resume_hart(uint32_t hart_id)
{
    if (!coro_state.initialized || hart_id >= coro_state.n_hart) {
        fprintf(stderr, "coro_resume_hart: invalid hart_id=%u\n", hart_id);
        return;
    }

    coro_t *co = coro_state.coroutines[hart_id];
    if (!co || !co->context) {
        fprintf(stderr, "coro_resume_hart: hart %u has no coroutine\n",
                hart_id);
        return;
    }

    if (co->state != CORO_STATE_SUSPENDED) {
        fprintf(stderr, "coro_resume_hart: hart %u not suspended (state=%d)\n",
                hart_id, co->state);
        return;
    }

    /* Check for stack overflow before resuming */
    coro_check_stack(co);

    coro_state.current_hart = hart_id;
    co->state = CORO_STATE_RUNNING;
    jump_into(co);

    /* Check for stack overflow after returning from coroutine */
    coro_check_stack(co);
}

void coro_yield(void)
{
    if (!coro_state.initialized) {
        fprintf(stderr, "coro_yield: not initialized\n");
        return;
    }

    coro_t *co = tls_running_coro;
    if (!co) {
        fprintf(stderr, "coro_yield: no running coroutine\n");
        return;
    }

    if (co->state != CORO_STATE_RUNNING) {
        fprintf(stderr, "coro_yield: coroutine not running\n");
        return;
    }

    co->state = CORO_STATE_SUSPENDED;
    jump_out(co);
}

bool coro_is_suspended(uint32_t hart_id)
{
    if (!coro_state.initialized || hart_id >= coro_state.n_hart)
        return false;

    coro_t *co = coro_state.coroutines[hart_id];
    if (!co || !co->context)
        return false;

    return (co->state == CORO_STATE_SUSPENDED);
}

uint32_t coro_current_hart_id(void)
{
    return coro_state.current_hart;
}
