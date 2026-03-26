# Growable Wake Stacks — Design

**Date:** March 26, 2026
**Status:** Design — no code changes yet
**Depends on:** Wake scheduler, x86_64 + arm64 context switch

---

## The Problem

Fixed 256KB stacks limit ShelbyC to ~32,000 wakes per GB of RAM. Go starts goroutines at 8KB and grows on demand, allowing millions per GB. Our dining philosophers use maybe 200 bytes of stack. Allocating 256KB for 200 bytes of actual usage is 1000x waste.

---

## The Design

### Initial allocation: 4KB

Every wake starts with a 4KB stack (one page). This is the minimum useful size — enough for the entry trampoline, one function frame, and a few local variables. A wake that does `ch <- 42` fits in 4KB easily.

```
Layout of a 4KB wake stack:
┌─────────────────┐ ← stack_hi (grows downward)
│ entry trampoline│
│ fn(arg) frame   │
│ local variables │
│                 │
│   (free space)  │
│                 │
├─────────────────┤ ← guard zone (checked, not mprotected)
│  guard canary   │
└─────────────────┘ ← stack_lo
```

### Growth trigger: stack check prologue

Every function prologue checks if there's enough stack space for the function's frame. If not, the runtime allocates a larger stack, copies the old stack, and updates pointers.

```c
// Compiler inserts this at every function entry:
void __sc_stack_check(uint64_t frame_size) {
    ScW *gp = sc_m->curw;
    uint64_t sp = get_stack_pointer();
    uint64_t limit = (uint64_t)gp->stack_lo + STACK_GUARD_SIZE;

    if (sp - frame_size < limit) {
        __sc_stack_grow(gp, frame_size);
    }
}
```

### Growth strategy: double

When a stack needs to grow:
1. Allocate new stack = 2× current size
2. Copy old stack contents to new stack (at the same relative position from top)
3. Update `stack_lo` and `stack_hi` on ScW
4. Adjust SP to point into the new stack
5. Free the old stack

```
Before growth (4KB, full):
┌─────────────┐ ← stack_hi = old_base + 4KB
│ frame A     │
│ frame B     │
│ frame C     │ ← SP is here, near the bottom
│ guard       │
└─────────────┘ ← stack_lo = old_base

After growth (8KB):
┌─────────────┐ ← stack_hi = new_base + 8KB
│             │
│ (new space) │
│             │
│ frame A     │ ← copied from old stack
│ frame B     │
│ frame C     │ ← SP adjusted to same offset from top
│             │
│ guard       │
└─────────────┘ ← stack_lo = new_base
```

### Size progression: 4KB → 8KB → 16KB → 32KB → 64KB → 128KB → 256KB → ...

Each growth doubles. Most wakes never grow past 4KB. Recursive functions might hit 16-32KB. The self-hosted compiler (deep AST recursion) might hit 256KB. No upper limit — it keeps doubling until OOM.

---

## Implementation Plan

### Phase 1: Stack check prologue (codegen)

**File:** `src/cg_decl.c` (function entry)

Every function gets a stack check at entry. The check compares SP against the wake's stack limit.

```c
// In cg_fn_decl, after entry block setup:

// Emit stack check prologue
LLVMValueRef frame_size = LLVMConstInt(i64_ty, estimated_frame_bytes, false);
LLVMTypeRef check_fn_ty = LLVMFunctionType(void_ty, (LLVMTypeRef[]){i64_ty}, 1, false);
LLVMValueRef check_fn = get_or_declare("__sc_stack_check", check_fn_ty);
LLVMBuildCall2(builder, check_fn_ty, check_fn, (LLVMValueRef[]){frame_size}, 1, "");
```

The `estimated_frame_bytes` is computed from the function's local variable count and types. Doesn't need to be exact — overestimating is safe (triggers growth earlier), underestimating is caught by the guard check.

**Optimization:** Functions marked `#[no_stack_check]` skip the prologue. Leaf functions with small frames (< 128 bytes) can skip it too — the remaining stack space is always enough.

### Phase 2: Stack growth runtime function

**File:** `src/runtime_sched.c`

```c
#define SC_STACK_INITIAL    (4 * 1024)     /* 4KB initial */
#define SC_STACK_GUARD      64             /* 64 bytes guard zone */

void __sc_stack_grow(ScW *gp, uint64_t needed) {
    uint64_t old_size = (uint64_t)gp->stack_hi - (uint64_t)gp->stack_lo;
    uint64_t new_size = old_size * 2;

    /* Keep doubling until the new size fits the needed frame */
    while (new_size < old_size + needed + SC_STACK_GUARD) {
        new_size *= 2;
    }

    /* Allocate new stack */
    void *new_mem = mmap(NULL, new_size + SC_STACK_GUARD_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_mem == MAP_FAILED) {
        fprintf(stderr, "stack overflow: wake %llu needs %llu bytes\n",
                gp->wid, new_size);
        abort();
    }

    /* Guard page at bottom */
    mprotect(new_mem, SC_STACK_GUARD_SIZE, PROT_NONE);

    void *new_lo = (char *)new_mem + SC_STACK_GUARD_SIZE;
    void *new_hi = (char *)new_lo + new_size;

    /* Copy old stack contents to new stack.
     * Stack grows downward, so used portion is at the TOP of old stack.
     * Copy it to the TOP of new stack (same offset from stack_hi). */
    uint64_t sp = get_stack_pointer();
    uint64_t used = (uint64_t)gp->stack_hi - sp;
    void *new_sp = (char *)new_hi - used;
    memcpy(new_sp, (void *)sp, used);

    /* Free old stack */
    void *old_mem = (char *)gp->stack_lo - SC_STACK_GUARD_SIZE;
    munmap(old_mem, old_size + SC_STACK_GUARD_SIZE);

    /* Update wake's stack bounds */
    gp->stack_lo = new_lo;
    gp->stack_hi = new_hi;

    /* Adjust SP to point into new stack.
     * This is the tricky part — we're ON the old stack when this runs.
     * Solution: __sc_stack_grow is called from the stack check prologue,
     * which is in the function being entered. After grow returns, the
     * function's frame hasn't been set up yet — SP is still at the
     * call site. We need to adjust SP before returning. */
    set_stack_pointer((uint64_t)new_sp);
}
```

### The Hard Part: Adjusting SP mid-function

The stack check runs at function entry, before the frame is set up. If we need to grow:

1. We're executing ON the current (old) stack
2. We allocate a new stack
3. We copy the old stack to the new one
4. We need to set SP to point into the new stack
5. Then return to the caller — but the return address is on the OLD stack (now copied to new)

This requires assembly helpers:

```asm
// arm64
__sc_stack_grow_asm:
    // x0 = ScW*, x1 = needed
    // Save LR (return address) — it's on the old stack
    mov x2, lr
    // Call C grow function (does mmap, memcpy, munmap)
    bl __sc_stack_grow_c
    // x0 now contains the new SP
    mov sp, x0
    // Restore LR from preserved register
    mov lr, x2
    ret

// x86_64
__sc_stack_grow_asm:
    // rdi = ScW*, rsi = needed
    // Save return address
    pop %rax          // return address
    push %rax         // put it back (grow_c will copy it)
    call __sc_stack_grow_c
    // rax = new SP
    mov %rax, %rsp
    ret               // pops the copied return address from new stack
```

### Phase 3: Reduce initial allocation

**File:** `src/runtime_sched.h`

```c
#define SC_STACK_SIZE       (4 * 1024)     /* 4KB initial (was 256KB) */
#define SC_STACK_GUARD_SIZE (4096)         /* Guard page */
```

**File:** `src/runtime_sched.c` — `w_alloc()`

No change needed — it already uses `SC_STACK_SIZE`. Just changing the constant reduces initial allocation from 256KB to 4KB per wake.

### Phase 4: Skip stack check for tiny functions

**File:** `src/cg_decl.c`

Leaf functions (no calls) with small frames don't need the check:

```c
bool needs_stack_check = true;
// Skip for leaf functions with small frames
if (no_calls && estimated_frame < 128) {
    needs_stack_check = false;
}
// Skip for extern fn declarations (no body)
if (!node->u.func.body) {
    needs_stack_check = false;
}
```

---

## Pointer Adjustment Problem

When the stack moves, any pointers into the stack become dangling. This is the reason Go's stack growth works — Go controls all pointer creation and can adjust them. In ShelbyC:

### What's safe:
- Return addresses (on the stack, copied with it)
- Frame pointers (on the stack, copied with it)
- Local variables by value (on the stack, copied)
- Callee-saved registers (in ScContext, not on stack during growth)

### What's dangerous:
- Pointers to stack-allocated variables passed to other functions
- References (`&x`) where `x` is on the stack

### Solution: Don't move the stack if references exist

The simple rule: **if a function takes a reference to a local variable, that variable must be heap-allocated (escape analysis) or the stack can't grow (pin it).**

For the initial implementation, we can be conservative:
- If a function takes `&` of any local, the stack check is skipped for that function
- The function uses whatever stack space is available
- If it overflows, the guard page catches it (crash, not corruption)

Later, escape analysis can determine which locals actually escape and only heap-allocate those.

### Alternative: Segmented stacks (no copying)

Instead of copying the old stack to a bigger one, allocate a NEW segment and chain them:

```
Segment 1 (4KB):        Segment 2 (8KB):
┌─────────────┐         ┌─────────────────────┐
│ frame A     │         │ frame D             │
│ frame B     │         │ frame E             │
│ frame C     │    ──→  │ (current execution) │
│ [link to 2] │         │                     │
└─────────────┘         └─────────────────────┘
```

Pros: No copying, no pointer adjustment. Each segment is independent.
Cons: Hot-split problem (function at segment boundary causes repeated alloc/free). Go abandoned this for contiguous stacks in Go 1.4.

### Recommendation: Start with segmented, move to contiguous later

Segmented stacks avoid the pointer problem entirely. The hot-split problem is real but rare — most functions don't sit exactly at the boundary. And for our use case (100k simple workers), segmented stacks are perfect because most wakes never need a second segment.

```c
typedef struct StackSegment {
    struct StackSegment *prev;  /* previous segment (linked list) */
    uint64_t size;              /* size of this segment */
    /* stack data follows */
} StackSegment;
```

The stack check:
1. Check if current segment has room
2. If not, allocate new segment (double the size)
3. Set SP to top of new segment
4. Link new segment to old one
5. When function returns past segment boundary, free the segment and restore old SP

---

## Memory Impact

| Stack strategy | Per wake | 100K wakes | 1M wakes |
|---------------|----------|------------|----------|
| Fixed 256KB | 256 KB | 25 GB | 250 GB |
| Fixed 4KB | 4 KB | 400 MB | 4 GB |
| Growable 4KB start | 4 KB (idle) | 400 MB (idle) | 4 GB (idle) |
| Go (8KB start) | 8 KB (idle) | 800 MB (idle) | 8 GB (idle) |

With 4KB initial stacks:
- Your M4 Pro (24GB): ~6 million wakes
- Your tower (128GB): ~32 million wakes
- Practical limit is scheduling overhead, not memory

---

## Implementation Order

| Phase | What | Risk | Effort |
|-------|------|------|--------|
| 1 | Reduce SC_STACK_SIZE to 4KB | Low — just a constant | 5 min |
| 2 | Add __sc_stack_check call at function entry | Medium — codegen change | 2 hours |
| 3 | Implement __sc_stack_grow with segmented stacks | Medium — assembly + C | 4 hours |
| 4 | Skip stack check for leaf/tiny functions | Low — optimization | 1 hour |
| 5 | Test at scale (100K, 1M wakes) | — | 1 hour |
| 6 | Contiguous stack growth (optional, replaces segmented) | High — pointer adjustment | 8 hours |

**Total for segmented: ~8 hours**
**Total including contiguous: ~16 hours**

Start with Phase 1 (just reduce the constant) and see how far it goes. If 4KB fixed stacks handle 100K wakes without stack overflow, we might not need growth at all for the common case. Add growth only when we find a real program that overflows 4KB.

---

*The 200-byte wake doesn't need a 256KB stack. Give it 4KB. If it needs more, it'll ask.*
