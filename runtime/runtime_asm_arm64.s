/*
 * ShelbyC Runtime — arm64 Context Switch
 *
 * Two functions:
 *   __sc_context_switch(from, to) — save current context, restore target
 *   __sc_context_entry()          — trampoline for new wake entry
 *
 * Saves/restores all callee-saved registers per AAPCS64:
 *   GP:   x19-x28, x29 (FP), x30 (LR)
 *   SIMD: d8-d15
 *   SP:   stack pointer
 *
 * NOTE: x18 is reserved by macOS — never touched.
 *
 * ScContext layout (from runtime_sched.h):
 *   offset  0: sp   (uint64_t)
 *   offset  8: lr   (uint64_t)  — x30
 *   offset 16: fp   (uint64_t)  — x29
 *   offset 24: x[0] (uint64_t)  — x19
 *   offset 32: x[1] (uint64_t)  — x20
 *   offset 40: x[2] (uint64_t)  — x21
 *   offset 48: x[3] (uint64_t)  — x22
 *   offset 56: x[4] (uint64_t)  — x23
 *   offset 64: x[5] (uint64_t)  — x24
 *   offset 72: x[6] (uint64_t)  — x25
 *   offset 80: x[7] (uint64_t)  — x26
 *   offset 88: x[8] (uint64_t)  — x27
 *   offset 96: x[9] (uint64_t)  — x28
 *   offset 104: d[0] (uint64_t) — d8
 *   offset 112: d[1] (uint64_t) — d9
 *   offset 120: d[2] (uint64_t) — d10
 *   offset 128: d[3] (uint64_t) — d11
 *   offset 136: d[4] (uint64_t) — d12
 *   offset 144: d[5] (uint64_t) — d13
 *   offset 152: d[6] (uint64_t) — d14
 *   offset 160: d[7] (uint64_t) — d15
 */

/* ScContext field offsets */
#define CTX_SP   0
#define CTX_LR   8
#define CTX_FP   16
#define CTX_X19  24
#define CTX_X20  32
#define CTX_X21  40
#define CTX_X22  48
#define CTX_X23  56
#define CTX_X24  64
#define CTX_X25  72
#define CTX_X26  80
#define CTX_X27  88
#define CTX_X28  96
#define CTX_D8   104
#define CTX_D9   112
#define CTX_D10  120
#define CTX_D11  128
#define CTX_D12  136
#define CTX_D13  144
#define CTX_D14  152
#define CTX_D15  160

.text
.align 4

/*
 * void __sc_context_switch(ScContext *from, ScContext *to)
 *
 * x0 = pointer to ScContext to save INTO  (current wake)
 * x1 = pointer to ScContext to restore FROM (target wake)
 *
 * Saves all callee-saved registers into *from, then restores
 * all callee-saved registers from *to and jumps to to->lr.
 *
 * After this returns (via ret), execution continues at the
 * PC that was saved in to->lr.
 */
.globl ___sc_context_switch
___sc_context_switch:
    /* ---- Save current context into *from (x0) ---- */

    /* Save stack pointer */
    mov     x2, sp
    str     x2, [x0, #CTX_SP]

    /* Save link register and frame pointer */
    str     x30, [x0, #CTX_LR]
    str     x29, [x0, #CTX_FP]

    /* Save callee-saved GP registers x19-x28 */
    stp     x19, x20, [x0, #CTX_X19]
    stp     x21, x22, [x0, #CTX_X21]
    stp     x23, x24, [x0, #CTX_X23]
    stp     x25, x26, [x0, #CTX_X25]
    stp     x27, x28, [x0, #CTX_X27]

    /* Save callee-saved SIMD registers d8-d15 */
    stp     d8,  d9,  [x0, #CTX_D8]
    stp     d10, d11, [x0, #CTX_D10]
    stp     d12, d13, [x0, #CTX_D12]
    stp     d14, d15, [x0, #CTX_D14]

    /* ---- Restore target context from *to (x1) ---- */

    /* Restore callee-saved SIMD registers d8-d15 */
    ldp     d8,  d9,  [x1, #CTX_D8]
    ldp     d10, d11, [x1, #CTX_D10]
    ldp     d12, d13, [x1, #CTX_D12]
    ldp     d14, d15, [x1, #CTX_D14]

    /* Restore callee-saved GP registers x19-x28 */
    ldp     x19, x20, [x1, #CTX_X19]
    ldp     x21, x22, [x1, #CTX_X21]
    ldp     x23, x24, [x1, #CTX_X23]
    ldp     x25, x26, [x1, #CTX_X25]
    ldp     x27, x28, [x1, #CTX_X27]

    /* Restore frame pointer and link register */
    ldr     x29, [x1, #CTX_FP]
    ldr     x30, [x1, #CTX_LR]

    /* Restore stack pointer */
    ldr     x2, [x1, #CTX_SP]
    mov     sp, x2

    /* Jump to restored LR (x30). ret = br x30 */
    ret

/*
 * __sc_context_entry — trampoline for new wake startup.
 *
 * When a new wake is first switched to, its context has:
 *   LR  = __sc_context_entry
 *   x19 = fn   (the wake's entry function)
 *   x20 = arg  (the argument to fn)
 *
 * This trampoline calls fn(arg), then calls __sc_wake_exit()
 * to return the wake to the scheduler (marks it W_DEAD,
 * frees resources, schedules next wake).
 *
 * __sc_wake_exit() is implemented in C (runtime_sched.c)
 * and never returns.
 */
.globl ___sc_context_entry
___sc_context_entry:
    /* x19 = fn, x20 = arg (set up by sc_context_init) */
    mov     x0, x20         /* arg -> first parameter */
    blr     x19             /* call fn(arg) */

    /* fn returned — wake is done */
    bl      ___sc_wake_exit  /* never returns */
    brk     #1              /* unreachable — trap if somehow reached */
