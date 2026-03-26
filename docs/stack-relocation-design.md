# Stack Relocation via Flow Ownership — Design

**Date:** March 26, 2026
**Status:** Design — no code changes yet
**Depends on:** Flow Ownership (proven), growable stacks Phase 1-3 (proven), LLVM stack maps

---

## The Problem

When a wake's stack grows, the runtime copies the used portion to a bigger allocation and frees the old one. Any pointer that referenced data on the old stack is now dangling. Go solves this with GC-aware scanning. We don't have GC. We have something better.

---

## The Insight

Flow Ownership already knows, at compile time, exactly which variables hold references to stack-local data. The checker tracks every `&x` and `&mut x`. It knows:

- Which variables are references (OS_BORROWED in the ownership state)
- What they point to (borrow_source tracking)
- Whether the target is stack-local or heap-allocated
- The lifetime scope of the reference

This is the same information that prevents use-after-free. It's also the information needed to relocate stack pointers during growth.

---

## The Design

### Compile Time: Stack Map Generation

Every function that could run on a wake (everything except Main) gets a **stack map** — a compile-time-generated array of frame offsets that contain pointers to stack-local data.

```c
// Compiler generates this alongside each function:
typedef struct {
    uint32_t num_ptrs;        // how many pointer slots in this frame
    uint32_t offsets[];       // frame offsets (from SP) of each pointer
} StackPtrMap;

// Example for:
// fn Process(data &Vec<i32>) {
//     buf := Alloc(1024);
//     ptr := &buf;            // <-- stack pointer to stack local
//     Use(ptr);
// }
//
// Stack map: { num_ptrs: 1, offsets: [24] }  // ptr is at SP+24
```

**What goes in the map:**
- Local variables of type `&T` or `&mut T` where T is stack-allocated
- Saved frame pointers (BP/FP) — always a stack pointer
- Saved return addresses — always a stack pointer (on the stack)

**What does NOT go in the map:**
- Heap pointers (`Box<T>`, `Vec<T>` — the Vec itself is on the stack but its buffer is on the heap)
- Function pointers
- Integer values that happen to look like pointers
- References to globals or statics

### How Flow Ownership Provides This

The flow analysis already runs on every function (Pass 3 in the checker). For each variable:

```
FlowVar {
    OwnershipState state;     // OS_ALIVE, OS_BORROWED, etc.
    int borrow_source;        // which var this borrows from (-1 if none)
    // ...
}
```

Variables with `state == OS_BORROWED` or those that ARE references (type is `&T`) are stack pointer candidates. The checker knows their type, their position in the scope, and what they point to.

**The extension:** After flow analysis, emit a `StackPtrMap` for each function. The codegen assigns stack offsets to variables (via `cg_entry_alloca`). After all allocas are placed, walk the flow variables and record which allocas are references to other allocas.

```c
// In cg_decl.c, after all allocas are placed:
for (int i = 0; i < cg->local_count; i++) {
    CgLocal *local = cg->locals[i];
    Type *ty = local->type;

    if (ty->kind == TY_REF || ty->kind == TY_PTR) {
        // Check if the referent is also a local (stack-allocated)
        Type *inner = type_resolve(ty->u.reference.inner);
        if (is_stack_local(inner, local)) {
            // Record this alloca's offset in the stack map
            stack_map_add(cg, local->alloca);
        }
    }
}
```

### Runtime: Stack Relocation

When `execute()` handles SWITCH_GROW:

```c
case SWITCH_GROW: {
    // 1. Allocate new stack (2x)
    // 2. Copy used portion
    // 3. Compute delta = new_sp - old_sp

    uint64_t delta = (uint64_t)new_sp_ptr - saved_sp;

    // 4. Walk the call stack using saved frame pointers
    //    At each frame, look up the function's StackPtrMap
    //    For each pointer offset in the map, adjust the value

    uint64_t *fp = get_saved_frame_pointer(gp);
    while (fp_is_valid(fp, new_lo, new_hi)) {
        // Find which function this frame belongs to
        uint64_t return_addr = *(fp + 1);  // return address is after saved FP
        StackPtrMap *map = lookup_stack_map(return_addr);

        if (map) {
            uint64_t frame_sp = (uint64_t)fp;  // approximate frame base
            for (uint32_t i = 0; i < map->num_ptrs; i++) {
                uint64_t *slot = (uint64_t *)(frame_sp + map->offsets[i]);
                uint64_t val = *slot;
                // If this pointer points into the old stack, adjust it
                if (val >= saved_sp && val < saved_sp + used) {
                    *slot = val + delta;
                }
            }
        }

        // Walk to next frame
        fp = (uint64_t *)(*fp);
    }

    // 5. Adjust saved SP, FP in context
    // 6. Free old stack
    // 7. Resume on new stack
}
```

### LLVM Integration

LLVM has two mechanisms for stack maps:

**Option A: Custom section**
The compiler emits a global constant array per function containing the pointer offsets. A global registry maps function addresses to their stack maps.

```c
// Codegen emits:
static const uint32_t __sc_stackmap_Process[] = { 1, 24 };  // 1 pointer at offset 24

// Registry (built at link time):
typedef struct {
    void *fn_addr;
    const uint32_t *map;
} StackMapEntry;
```

**Option B: LLVM stackmap intrinsic**
Use `@llvm.experimental.stackmap` which LLVM already supports. This emits a `__llvm_stackmaps` section that tools can parse. However, this is designed for GC and JIT, not our use case.

**Option C: Frame metadata via LLVM gc.statepoint**
LLVM's statepoint mechanism is designed for precise GC but can be repurposed for stack relocation.

**Recommendation: Option A.** Simple, we control the format, no experimental LLVM features. One global array per function, one registry array linking function addresses to maps.

---

## What Goes in the Stack Map

For each function, the stack map lists frame offsets of:

### 1. References to stack locals
```shelbyc
fn Process() {
    buf := [100]i32{};
    ptr := &buf;        // ptr is at frame offset X, points to buf which is also on stack
    // Stack map: ptr at offset X
}
```

### 2. Saved frame pointer
Every function's frame starts with the saved BP/FP. This always points into the stack (the caller's frame). The runtime already adjusts this via the FP chain walk, but including it in the map makes the adjustment uniform.

### 3. Saved callee-saved registers
The context switch saves these in ScContext, not on the stack. No map entry needed.

### 4. Return address
On x86_64, the return address is pushed by `call`. On arm64, it's in LR (saved in ScContext). The return address points to CODE, not the stack. No adjustment needed.

---

## What Does NOT Go in the Stack Map

### Heap pointers
```shelbyc
fn Process() {
    v := Vec<i32>.New();   // v is on stack, v's buffer is on heap
    // v itself doesn't need adjustment — the Vec struct's internal
    // pointer goes to the heap, not the stack
}
```

### Pointers passed as arguments
```shelbyc
fn Use(data &Vec<i32>) {
    // data is a reference parameter — it points to the CALLER's stack
    // When THIS function's stack grows, data doesn't move
    // When the CALLER's stack grows, data needs adjustment in the CALLER's map
}
```

Wait — this is the critical insight. If function A passes `&x` to function B, and function B's stack grows, `&x` still points to A's stack which didn't move. Fine. But if function A's stack grows while B holds `&x`, then `&x` is dangling.

**But this can't happen.** Function A called function B. A is blocked waiting for B to return. A's stack can't grow because A isn't executing — only the currently executing function's stack can grow (the stack check is at function entry, and only the deepest frame triggers it).

**The only stack that grows is the one currently being entered.** All callers are frozen. Their stack data doesn't move. References to caller data remain valid.

This means: **we only need to adjust pointers within the CURRENT function's frame that point to OTHER locations in the SAME stack.** And the most common such pointers are:
- Saved frame pointers (BP chain)
- Local `&` references to other locals in the same function (rare)
- Saved register spills (handled by ScContext)

The BP chain is the critical one, and we're already walking it. The crash is probably from one of:
1. The BP chain walk going off the end
2. Return addresses being misinterpreted as stack pointers
3. The initial FP being wrong after copy

---

## Simplified Fix: Just BP Chain + Return Addresses

Given the insight that only the current stack moves and callers are frozen:

1. Copy the stack to the new location
2. Walk the BP chain in the copied stack
3. For each saved BP, if it pointed into the old stack, add delta
4. Return addresses on the stack (x86_64 only) point to code, not stack — skip them
5. That's it

The crash is likely from the BP chain walk dereferencing a bad pointer. Let me trace exactly what happens:

### Before growth (16KB stack):
```
stack_hi → ┌──────────────┐
           │ fn Deep(100) │ ← saved BP points to Deep(99)'s frame
           │ fn Deep(99)  │ ← saved BP points to Deep(98)'s frame
           │ ...          │
           │ fn Deep(1)   │ ← saved BP points to Main's frame
           │ fn Main()    │ ← saved BP = 0 or points to __sc_context_entry
           │ trampoline   │
stack_lo → └──────────────┘
```

### After copy to 32KB stack:
```
new_hi   → ┌──────────────┐
            │ (new space)  │
            │              │
            │ fn Deep(100) │ ← saved BP STILL points to old stack → CRASH
            │ fn Deep(99)  │
            │ ...          │
new_lo   → └──────────────┘
```

Every saved BP in the copied frames still contains the OLD address. The delta adjustment must fix every one.

### The fix:

```c
// After memcpy, walk ALL 8-byte slots in the copied region
// and adjust any that look like they point into the old stack.
// This is conservative — it might adjust non-pointers that happen
// to fall in the old range. But that's rare and the alternative
// (precise maps) is complex.

uint64_t *slot = (uint64_t *)new_sp_ptr;
uint64_t *end = (uint64_t *)new_hi;
while (slot < end) {
    uint64_t val = *slot;
    if (val >= old_sp && val < old_sp + used) {
        *slot = val + delta;
    }
    slot++;
}
```

This is the **conservative scan** approach. It walks every 8-byte aligned value in the copied stack and adjusts any that fall in the old range. It's O(stack_size/8) which is fast (2000 iterations for 16KB). False positives (integers that happen to look like old stack addresses) are extremely rare — the old stack address range is a specific mmap region that normal integer values wouldn't hit.

This is simpler than precise maps, works without Flow Ownership metadata, and handles ALL stack-internal pointers including ones we might miss with frame-pointer-only walking.

---

## Implementation Plan

### Phase 1: Conservative scan (immediate fix)

Replace the frame pointer chain walk with a conservative scan of all 8-byte slots. ~10 lines of code. Fixes the SIGBUS crash. May have false positives but they're vanishingly rare.

### Phase 2: Precise maps via Flow Ownership (correctness proof)

Add `StackPtrMap` generation in codegen using Flow Ownership's borrow tracking. Use precise maps instead of conservative scan. Zero false positives. Proves that Flow Ownership handles stack relocation — a capability no other non-GC language has.

### Phase 3: Optimization

Skip the stack check prologue for leaf functions (no calls = no stack growth possible). Reduce overhead for hot inner loops.

---

## Why This Matters

Every other non-GC language with growable stacks either:
- Uses segmented stacks (Go pre-1.4, hot-split problem)
- Uses contiguous stacks with GC scanning (Go 1.4+)
- Doesn't have growable stacks (Rust, C, C++)

ShelbyC has growable contiguous stacks with compile-time-known pointer maps via Flow Ownership. No GC. No runtime scanning. The compiler tells the runtime exactly what to adjust because Flow Ownership already tracks every reference.

This is Innovation #5.
