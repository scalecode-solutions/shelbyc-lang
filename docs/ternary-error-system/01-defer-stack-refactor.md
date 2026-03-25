# Part 1: Defer Stack Refactor

**Goal:** Replace `AstNode **defers` with `DeferEntry *defers` in CgScope. Zero behavior change. All 768 Furling Gate tests must pass identically.

**Files modified:** `codegen.h`, `cg_internal.h`, `cg_stmt.c`

---

## Step 1.1: New DeferEntry struct

**File:** `src/codegen.h`
**Location:** Inside `struct CgScope` (line 28-47)

### Before (current code):
```c
struct CgScope {
    CgLocal  **buckets;
    int        bucket_count;
    CgScope   *parent;

    /* Drop tracking: locals that need Drop() at scope exit, in declaration order */
    struct DropEntry {
        LLVMValueRef alloca;      /* the variable's alloca */
        Type       *ty;        /* the variable's type */
        LLVMValueRef drop_flag;   /* i1 alloca: true = needs drop */
    } *drops;
    int drop_count;
    int drop_cap;

    /* Defer tracking: deferred expressions for this scope (LIFO) */
    AstNode **defers;
    int defer_count;
    int defer_cap;
};
```

### After:
```c
/* Ternary defer entry: one struct for defer, errdefer, and recovery-defer.
 * trigger is a trit stored as i8:
 *   -1 (N) = errdefer: runs on error path only
 *    0 (Z) = defer:    runs on error and normal paths (not recovery)
 *   +1 (P) = recovery: runs on recovery path only (conditions/restarts)
 *
 * err_capture is for errdefer |e| { ... } — NULL when not used. */
typedef struct {
    AstNode  *expr;         /* the deferred expression or block */
    int8_t    trigger;      /* N(-1), Z(0), P(+1) */
    AstNode  *err_capture;  /* for errdefer |e| { ... } — binding node, NULL if none */
} DeferEntry;

struct CgScope {
    CgLocal  **buckets;
    int        bucket_count;
    CgScope   *parent;

    /* Drop tracking: locals that need Drop() at scope exit, in declaration order */
    struct DropEntry {
        LLVMValueRef alloca;
        Type        *ty;
        LLVMValueRef drop_flag;
    } *drops;
    int drop_count;
    int drop_cap;

    /* Ternary defer stack: unified defer + errdefer + recovery-defer (LIFO) */
    DeferEntry *defers;
    int defer_count;
    int defer_cap;
};
```

**What changed:**
- Added `DeferEntry` typedef with `expr`, `trigger` (trit), `err_capture`
- Changed `AstNode **defers` to `DeferEntry *defers`
- Comments updated to describe the ternary system

**What did NOT change:**
- `defer_count` and `defer_cap` — same int counters
- `drops` array — completely untouched
- `CgLocal`, `buckets`, `parent` — untouched

---

## Step 1.2: Add propagated_err_val to Codegen struct

**File:** `src/codegen.h`
**Location:** Inside the `Codegen` typedef (line 51-139)

### Add after `int mono_param_count;` (line 122):
```c
    /* Ternary error system: error value being propagated (for errdefer |e| capture) */
    LLVMValueRef     propagated_err_val;  /* set on N path, read by errdefer emit */
```

This field is set when the `?` operator enters the error branch (cg_literal.c) and read by `cg_emit_scope_defers_trit` when emitting errdefers with error capture.

---

## Step 1.3: Update cg_push_defer

**File:** `src/cg_stmt.c`
**Location:** Lines 40-52

### Before:
```c
/* Push a deferred expression onto the current scope's defer stack */
void cg_push_defer(Codegen *cg, AstNode *expr) {
    CgScope *s = cg->scope;
    if (s->defer_count >= s->defer_cap) {
        int new_cap = s->defer_cap < 8 ? 8 : s->defer_cap * 2;
        AstNode **new_stack = arena_alloc(cg->arena, sizeof(AstNode *) * new_cap);
        if (s->defers)
            memcpy(new_stack, s->defers, sizeof(AstNode *) * s->defer_count);
        s->defers = new_stack;
        s->defer_cap = new_cap;
    }
    s->defers[s->defer_count++] = expr;
}
```

### After:
```c
/* Push a ternary defer entry onto the current scope's defer stack.
 * trigger: -1(N)=errdefer, 0(Z)=defer, +1(P)=recovery-defer
 * err_capture: binding node for errdefer |e| { ... }, NULL if none */
static void cg_push_defer_entry(Codegen *cg, AstNode *expr, int8_t trigger,
                                 AstNode *err_capture) {
    CgScope *s = cg->scope;
    if (s->defer_count >= s->defer_cap) {
        int new_cap = s->defer_cap < 8 ? 8 : s->defer_cap * 2;
        DeferEntry *new_stack = arena_alloc(cg->arena, sizeof(DeferEntry) * new_cap);
        if (s->defers)
            memcpy(new_stack, s->defers, sizeof(DeferEntry) * s->defer_count);
        s->defers = new_stack;
        s->defer_cap = new_cap;
    }
    s->defers[s->defer_count++] = (DeferEntry){
        .expr = expr,
        .trigger = trigger,
        .err_capture = err_capture,
    };
}

/* Convenience: push a Z (normal defer) — matches current behavior exactly */
void cg_push_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 0, NULL);
}

/* Push an N (errdefer) with optional error capture */
void cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture) {
    cg_push_defer_entry(cg, expr, -1, err_capture);
}

/* Push a P (recovery-defer) */
void cg_push_recovery_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 1, NULL);
}
```

**What changed:**
- Internal `cg_push_defer_entry` takes trigger trit and err_capture
- `cg_push_defer` is now a wrapper that passes `trigger=0, err_capture=NULL`
- `sizeof(AstNode *)` → `sizeof(DeferEntry)` in the grow logic
- Two new functions: `cg_push_errdefer`, `cg_push_recovery_defer`

**Why this is safe:** `cg_push_defer(cg, expr)` has identical behavior to the old code. It pushes a DeferEntry with trigger=0 (Z), which the emit function treats exactly like the old code treated a bare AstNode pointer.

---

## Step 1.4: Update cg_emit_scope_defers

**File:** `src/cg_stmt.c`
**Location:** Lines 11-17

### Before:
```c
/* Emit deferred expressions for a single scope in LIFO order */
void cg_emit_scope_defers(Codegen *cg, CgScope *scope) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        cg_expr(cg, scope->defers[i]);
    }
}
```

### After:
```c
/* Emit deferred expressions for a single scope in LIFO order.
 * Filters by exit path trit:
 *
 *   trigger\exit   N(error)  Z(normal)  P(recovered)
 *   N (errdefer)   RUN       skip       skip
 *   Z (defer)      RUN       RUN        skip
 *   P (recovery)   skip      skip       RUN
 *
 * This is the core of the ternary defer system. One function,
 * one loop, one truth table. */
void cg_emit_scope_defers_trit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        DeferEntry *d = &scope->defers[i];

        /* Ternary truth table */
        bool should_run;
        switch (d->trigger) {
        case  0: should_run = (exit_path <= 0); break;  /* Z: runs on N(-1) and Z(0) */
        case -1: should_run = (exit_path == -1); break;  /* N: runs on N(-1) only */
        case  1: should_run = (exit_path == 1); break;   /* P: runs on P(+1) only */
        default: should_run = false; break;
        }

        if (!should_run) continue;

        /* If errdefer with error capture, bind the error value */
        if (d->trigger == -1 && d->err_capture && cg->propagated_err_val) {
            LLVMTypeRef err_ty = LLVMTypeOf(cg->propagated_err_val);
            LLVMValueRef err_alloca = cg_entry_alloca(cg, err_ty, "errdefer.capture");
            LLVMBuildStore(cg->builder, cg->propagated_err_val, err_alloca);
            /* Register as local so the errdefer body can reference it by name */
            Type *err_val_type = cg->cur_fn_ret;
            if (err_val_type && err_val_type->kind == TY_RESULT)
                err_val_type = type_resolve(err_val_type->u.result.err_type);
            cg_define_local(cg, d->err_capture->u.ident.name,
                           d->err_capture->u.ident.name_len,
                           err_alloca, err_val_type);
        }

        cg_expr(cg, d->expr);
    }
}

/* Backward-compatible wrapper: emit with Z (normal) exit path.
 * This is what all existing call sites use during the refactor step. */
void cg_emit_scope_defers(Codegen *cg, CgScope *scope) {
    cg_emit_scope_defers_trit(cg, scope, 0);
}
```

**What changed:**
- New `cg_emit_scope_defers_trit` takes `exit_path` trit parameter
- Truth table implemented as switch on `d->trigger`
- Error capture binding for errdefer `|e|` syntax
- Old `cg_emit_scope_defers` is now a wrapper passing Z(0)

**Why this is safe:** The wrapper `cg_emit_scope_defers(cg, scope)` calls `cg_emit_scope_defers_trit(cg, scope, 0)`. With `exit_path=0` (Z), the truth table says:
- trigger=0 (Z): `0 <= 0` → true → RUN ✓ (same as before)
- trigger=-1 (N): `-1 == 0` → false → skip (no errdefers exist yet)
- trigger=+1 (P): `1 == 0` → false → skip (no recovery-defers exist yet)

Since all existing defers have trigger=0, and the wrapper passes exit_path=0, every defer runs exactly as before.

---

## Step 1.5: Update cg_emit_scope_cleanup and friends

**File:** `src/cg_stmt.c`
**Location:** Lines 19-38

### Before:
```c
/* Emit defers then drops for a single scope */
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_defers(cg, scope);
    cg_emit_scope_drops(cg, scope);
}

/* Emit cleanup for all scopes from current to fn_scope (inclusive) */
void cg_emit_all_scope_cleanup(Codegen *cg) {
    for (CgScope *s = cg->scope; s; s = s->parent) {
        cg_emit_scope_cleanup(cg, s);
        if (s == cg->fn_scope) break;
    }
}

/* Emit cleanup for scopes from current up to (but not including) stop */
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_cleanup(cg, s);
    }
}
```

### After:
```c
/* Emit defers then drops for a single scope with exit path trit */
void cg_emit_scope_cleanup_trit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    cg_emit_scope_defers_trit(cg, scope, exit_path);
    cg_emit_scope_drops(cg, scope);
}

/* Backward-compatible wrapper */
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_cleanup_trit(cg, scope, 0);  /* Z: normal */
}

/* Emit cleanup for all scopes with exit path trit */
void cg_emit_all_scope_cleanup_trit(Codegen *cg, int8_t exit_path) {
    for (CgScope *s = cg->scope; s; s = s->parent) {
        cg_emit_scope_cleanup_trit(cg, s, exit_path);
        if (s == cg->fn_scope) break;
    }
}

/* Backward-compatible wrapper */
void cg_emit_all_scope_cleanup(Codegen *cg) {
    cg_emit_all_scope_cleanup_trit(cg, 0);  /* Z: normal */
}

/* Emit cleanup for scopes from current up to (but not including) stop.
 * Always Z — break/continue are not error paths. */
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_cleanup_trit(cg, s, 0);  /* Z: normal */
    }
}
```

**What changed:**
- New `_trit` versions of each function that take `exit_path`
- Old functions become wrappers passing Z(0)
- `cg_emit_cleanup_to_scope` is always Z (break/continue aren't errors)

---

## Step 1.6: Update cg_internal.h function signatures

**File:** `src/cg_internal.h`
**Location:** Lines 95-102

### Before:
```c
void         cg_emit_scope_defers(Codegen *cg, CgScope *scope);
void         cg_emit_scope_cleanup(Codegen *cg, CgScope *scope);
void         cg_emit_all_scope_cleanup(Codegen *cg);
void         cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop);
void         cg_push_defer(Codegen *cg, AstNode *expr);
```

### After:
```c
/* Ternary defer system */
void         cg_emit_scope_defers_trit(Codegen *cg, CgScope *scope, int8_t exit_path);
void         cg_emit_scope_defers(Codegen *cg, CgScope *scope);  /* wrapper: Z */
void         cg_emit_scope_cleanup_trit(Codegen *cg, CgScope *scope, int8_t exit_path);
void         cg_emit_scope_cleanup(Codegen *cg, CgScope *scope);  /* wrapper: Z */
void         cg_emit_all_scope_cleanup_trit(Codegen *cg, int8_t exit_path);
void         cg_emit_all_scope_cleanup(Codegen *cg);  /* wrapper: Z */
void         cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop);  /* always Z */
void         cg_push_defer(Codegen *cg, AstNode *expr);  /* Z trigger */
void         cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture);  /* N trigger */
void         cg_push_recovery_defer(Codegen *cg, AstNode *expr);  /* P trigger */
```

---

## Step 1.7: Verify — zero behavior change

After making all changes in Steps 1.1-1.6:

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
```

**Expected:** 768/768 Furling Gate. Every test passes identically. No new warnings. No behavior change.

**Why:** All existing code calls the backward-compatible wrappers (`cg_push_defer`, `cg_emit_scope_defers`, `cg_emit_all_scope_cleanup`). These wrappers pass trigger=0 and exit_path=0, which produces identical behavior to the old code.

The new `_trit` functions and new push functions (`cg_push_errdefer`, `cg_push_recovery_defer`) exist but are not called by any existing code. They're tested in Part 3 (errdefer) and Part 4 (conditions/restarts).

---

## What this step accomplishes

1. **DeferEntry struct is in place** — ready for all three trit values
2. **The emit function has the truth table** — one function handles N/Z/P
3. **Error capture binding is ready** — the `|e|` mechanism exists
4. **propagated_err_val is on the Codegen struct** — ready for the ? operator to set it
5. **All existing behavior is preserved** — wrappers ensure zero regression
6. **The foundation is solid** — Parts 2-6 build on this without modifying Part 1's code

No shortcuts. No stubs. No "simplified for now." The ternary system is complete in data structure and logic. Only the callers haven't switched to the trit-aware paths yet — that's Parts 2-4.
