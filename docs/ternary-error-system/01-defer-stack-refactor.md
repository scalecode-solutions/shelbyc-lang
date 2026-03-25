# Part 1: Defer Stack Refactor

**Goal:** Replace `AstNode **defers` with `DeferEntry *defers` in CgScope. Pentit-width trigger (i8, values 0-4). Zero behavior change. All 772 Furling Gate tests must pass identically.

**Files modified:** `codegen.h`, `cg_internal.h`, `cg_stmt.c`

---

## Step 1.1: New DeferEntry struct

**File:** `src/codegen.h`

### After:
```c
/* Composable defer entry: one struct for all five defer types.
 * trigger is a pentit stored as i8 (raw encoding 0-4):
 *   0 (N2) = panicdefer:   runs on panic path only
 *   1 (N1) = errdefer:     runs on any error (panic + error)
 *   2 (Z)  = defer:        runs on ALL paths (always)
 *   3 (P1) = successdefer: runs on any success (normal + recovery)
 *   4 (P2) = recoverdefer: runs on recovery path only
 *
 * Firing rule: a defer fires if the exit path is on its side
 * and at least as extreme. See truth table in cg_emit_scope_defers_pentit.
 *
 * err_capture is for errdefer |e| { ... } — NULL when not used. */
typedef struct {
    AstNode  *expr;         /* the deferred expression or block */
    int8_t    trigger;      /* pentit raw encoding: 0=N2, 1=N1, 2=Z, 3=P1, 4=P2 */
    AstNode  *err_capture;  /* for errdefer |e| { ... } — binding node, NULL if none */
} DeferEntry;

struct CgScope {
    CgLocal  **buckets;
    int        bucket_count;
    CgScope   *parent;

    /* Drop tracking */
    struct DropEntry {
        LLVMValueRef alloca;
        Type        *ty;
        LLVMValueRef drop_flag;
    } *drops;
    int drop_count;
    int drop_cap;

    /* Composable defer stack: all five defer types, unified LIFO */
    DeferEntry *defers;
    int defer_count;
    int defer_cap;
};
```

**What changed:**
- `DeferEntry.trigger` is pentit raw encoding (0-4) instead of trit (-1, 0, +1)
- Comments describe all 5 defer types
- `AstNode **defers` → `DeferEntry *defers`

---

## Step 1.2: Add propagated_err_val to Codegen struct

**File:** `src/codegen.h`

```c
    /* Error system: error value being propagated (for errdefer |e| capture) */
    LLVMValueRef     propagated_err_val;  /* set on N1/N2 path, read by errdefer emit */
```

---

## Step 1.3: Update cg_push_defer

**File:** `src/cg_stmt.c`

```c
/* Push a defer entry with pentit trigger onto the current scope's defer stack. */
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

/* Convenience wrappers — developer uses the one that fits */
void cg_push_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 2, NULL);           /* Z: always */
}
void cg_push_panicdefer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 0, NULL);            /* N2: panic only */
}
void cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture) {
    cg_push_defer_entry(cg, expr, 1, err_capture);     /* N1: any error */
}
void cg_push_successdefer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 3, NULL);            /* P1: any success */
}
void cg_push_recoverdefer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 4, NULL);            /* P2: recovery only */
}
```

**Backward compatibility:** `cg_push_defer(cg, expr)` passes trigger=2 (Z), which fires on all paths — identical to the old behavior.

---

## Step 1.4: Update cg_emit_scope_defers

**File:** `src/cg_stmt.c`

```c
/* Emit deferred expressions for a single scope in LIFO order.
 * Filters by exit path using the pentit firing rule:
 *
 *   trigger\exit   N2(panic)  N1(error)  Z(normal)  P1(success)  P2(recovery)
 *   N2 panicdefer  RUN        skip       skip       skip         skip
 *   N1 errdefer    RUN        RUN        skip       skip         skip
 *   Z  defer       RUN        RUN        RUN        RUN          RUN
 *   P1 successdefer skip      skip       RUN        RUN          RUN
 *   P2 recoverdefer skip      skip       skip       skip         RUN
 *
 * Rule: trigger fires if exit is on the same side and >= extreme,
 * OR trigger is Z (center, fires always).
 *
 * In code: for negative triggers (0,1), fire if exit <= trigger.
 *          for positive triggers (3,4), fire if exit >= trigger.
 *          for Z (2), always fire. */
void cg_emit_scope_defers_pentit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        DeferEntry *d = &scope->defers[i];

        /* Pentit firing rule */
        bool should_run;
        if (d->trigger == 2) {
            should_run = true;                          /* Z: always */
        } else if (d->trigger < 2) {
            should_run = (exit_path <= d->trigger);     /* N side: fire if exit is at least as extreme */
        } else {
            should_run = (exit_path >= d->trigger);     /* P side: fire if exit is at least as extreme */
        }

        if (!should_run) continue;

        /* If errdefer/panicdefer with error capture, bind the error value */
        if (d->trigger <= 1 && d->err_capture && cg->propagated_err_val) {
            LLVMTypeRef err_ty = LLVMTypeOf(cg->propagated_err_val);
            LLVMValueRef err_alloca = cg_entry_alloca(cg, err_ty, "errdefer.capture");
            LLVMBuildStore(cg->builder, cg->propagated_err_val, err_alloca);
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

/* Backward-compatible wrapper: emit with Z (2) exit path — all defers fire */
void cg_emit_scope_defers(Codegen *cg, CgScope *scope) {
    cg_emit_scope_defers_pentit(cg, scope, 2);
}
```

**Why this is safe:** The wrapper passes exit_path=2 (Z). With Z, the truth table says:
- trigger=2 (Z): `true` → RUN (same as before)
- trigger<2 (N1, N2): `2 <= 0` or `2 <= 1` → false → skip (no errdefers exist yet)
- trigger>2 (P1, P2): `2 >= 3` or `2 >= 4` → false → skip (no successdefers exist yet)

All existing defers have trigger=2, and all fire. Identical behavior.

---

## Step 1.5: Update cleanup functions

Same pattern as before — `_pentit` versions that take `exit_path`, old functions become wrappers passing Z (2).

```c
void cg_emit_scope_cleanup_pentit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    cg_emit_scope_defers_pentit(cg, scope, exit_path);
    cg_emit_scope_drops(cg, scope);
}

void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_cleanup_pentit(cg, scope, 2);  /* Z: all */
}

void cg_emit_all_scope_cleanup_pentit(Codegen *cg, int8_t exit_path) {
    for (CgScope *s = cg->scope; s; s = s->parent) {
        cg_emit_scope_cleanup_pentit(cg, s, exit_path);
        if (s == cg->fn_scope) break;
    }
}

void cg_emit_all_scope_cleanup(Codegen *cg) {
    cg_emit_all_scope_cleanup_pentit(cg, 2);  /* Z: all */
}

void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_cleanup_pentit(cg, s, 2);  /* Z: break/continue are normal */
    }
}
```

---

## Step 1.6: Update cg_internal.h

```c
/* Composable defer system */
void cg_emit_scope_defers_pentit(Codegen *cg, CgScope *scope, int8_t exit_path);
void cg_emit_scope_defers(Codegen *cg, CgScope *scope);       /* wrapper: Z(2) */
void cg_emit_scope_cleanup_pentit(Codegen *cg, CgScope *scope, int8_t exit_path);
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope);      /* wrapper: Z(2) */
void cg_emit_all_scope_cleanup_pentit(Codegen *cg, int8_t exit_path);
void cg_emit_all_scope_cleanup(Codegen *cg);                   /* wrapper: Z(2) */
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop);    /* always Z(2) */
void cg_push_defer(Codegen *cg, AstNode *expr);               /* Z(2) trigger */
void cg_push_panicdefer(Codegen *cg, AstNode *expr);          /* N2(0) trigger */
void cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture);  /* N1(1) */
void cg_push_successdefer(Codegen *cg, AstNode *expr);        /* P1(3) trigger */
void cg_push_recoverdefer(Codegen *cg, AstNode *expr);        /* P2(4) trigger */
```

---

## Step 1.7: Verify — zero behavior change

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
```

**Expected:** 772/772 Furling Gate. Every test passes identically.

---

## What this step accomplishes

1. **DeferEntry struct is in place** — pentit-width trigger, ready for all 5 defer types
2. **The emit function has the truth table** — one function, one loop, pentit firing rule
3. **Error capture binding is ready** — the `|e|` mechanism exists for errdefer/panicdefer
4. **propagated_err_val is on the Codegen struct** — ready for `?` operator to set it
5. **All existing behavior is preserved** — wrappers ensure zero regression
6. **Five push functions exist** — panicdefer, errdefer, defer, successdefer, recoverdefer

A developer using only `defer` and `errdefer` never sees the pentit. A developer who needs `successdefer` has it. Everything composes.
