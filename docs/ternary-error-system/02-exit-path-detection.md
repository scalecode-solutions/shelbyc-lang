# Part 2: Exit Path Detection

**Goal:** Map every exit point in the C bootstrap codegen and assign it a pentit value. No new keywords, no new syntax — just change the existing `cg_emit_all_scope_cleanup(cg)` calls to `cg_emit_all_scope_cleanup_pentit(cg, exit_path)` with the correct pentit.

**Files modified:** `cg_stmt.c`, `cg_literal.c`

**Depends on:** Part 1 (DeferEntry struct and _pentit functions exist)

---

## Complete exit point inventory

There are exactly 7 places in the C bootstrap that call cleanup functions before exiting a scope or function. Each one gets a pentit value.

---

### Exit Point 1: Normal return (cg_stmt.c:345)

**Context:** `case ND_RETURN` — the user wrote `return expr;`

**File:** `src/cg_stmt.c`
**Line:** 345

#### Before:
```c
/* Emit deferred expressions and drops for all scopes before returning */
cg_emit_all_scope_cleanup(cg);
if (val) {
    /* Coerce return value to match function return type */
    LLVMTypeRef fn_ret_type = LLVMGetReturnType(
        LLVMGlobalGetValueType(cg->cur_fn));
    val = cg_coerce_int(cg, val, fn_ret_type);
    LLVMBuildRet(cg->builder, val);
} else
    LLVMBuildRetVoid(cg->builder);
```

#### After:
```c
/* Determine exit path pentit from the return expression */
int8_t exit_path = 2;  /* default: Z (normal) */
if (node->u.single.expr) {
    AstNode *ret_expr = node->u.single.expr;
    if (ret_expr->kind == ND_ERR) {
        /* return err(...) — explicit error return */
        exit_path = 1;  /* N1 (error) */
    } else if (ret_expr->kind == ND_OK) {
        /* return ok(...) — explicit ok return */
        exit_path = 2;   /* Z (normal) */
    } else {
        /* Could be returning a Result variable — check type */
        Type *ret_t = type_resolve(cg_expr_type(cg, ret_expr));
        if (ret_t && ret_t->kind == TY_RESULT) {
            /* Can't determine path at compile time — need runtime branch.
             * See "Runtime path detection" below. */
            cg_emit_result_path_branch(cg, val);
            /* val is now past the cleanup — just return it */
            LLVMTypeRef fn_ret_type = LLVMGetReturnType(
                LLVMGlobalGetValueType(cg->cur_fn));
            val = cg_coerce_int(cg, val, fn_ret_type);
            LLVMBuildRet(cg->builder, val);
            break;
        }
    }
}

/* Static path — known at compile time */
cg_emit_all_scope_cleanup_pentit(cg, exit_path);
if (val) {
    LLVMTypeRef fn_ret_type = LLVMGetReturnType(
        LLVMGlobalGetValueType(cg->cur_fn));
    val = cg_coerce_int(cg, val, fn_ret_type);
    LLVMBuildRet(cg->builder, val);
} else
    LLVMBuildRetVoid(cg->builder);
```

**Pentit value:**
- `return ok(...)` → Z (2)
- `return err(...)` → N1 (1)
- `return void` → Z (2)
- `return result_variable` → runtime branch (see below)

---

### Exit Point 2: TCO tail call return (cg_stmt.c:316)

**Context:** `case ND_RETURN` — self-recursive tail call detected, optimized to loop

**File:** `src/cg_stmt.c`
**Line:** 316

#### Before:
```c
cg_emit_all_scope_cleanup(cg);
LLVMBuildBr(cg->builder, cg->tco_loop_bb);
```

#### After:
```c
cg_emit_all_scope_cleanup_pentit(cg, 2);  /* Z: tail calls succeed */
LLVMBuildBr(cg->builder, cg->tco_loop_bb);
```

**Pentit value:** Always Z (2). A tail call is a successful continuation. If the tail call returns an error, the NEXT iteration handles it — this iteration succeeded in dispatching.

---

### Exit Point 3: ? propagation error branch (cg_literal.c:915)

**Context:** `case ND_PROPAGATE` — the `?` operator detected Result.is_ok=false, building the early return

**File:** `src/cg_literal.c`
**Line:** 915

#### Before:
```c
cg_emit_all_scope_cleanup(cg);
LLVMBuildRet(cg->builder, ret_val);
```

#### After:
```c
/* Store the error value for errdefer |e| capture */
cg->propagated_err_val = err_val;
cg_emit_all_scope_cleanup_pentit(cg, 1);  /* N1: error propagation */
cg->propagated_err_val = NULL;  /* clear after use */
LLVMBuildRet(cg->builder, ret_val);
```

**Pentit value:** Always N1 (1). The `?` operator error branch IS the error path by definition. This is the most important call site — it's where errdefer actually fires.

**The `propagated_err_val` assignment** is critical: it stores the error value so that `cg_emit_scope_defers_pentit` can bind it for `errdefer |e| { ... }` capture blocks.

---

### Exit Point 4: ? propagation void fallback (cg_literal.c:919)

**Context:** `case ND_PROPAGATE` — error branch for functions with void return type

**File:** `src/cg_literal.c`
**Line:** 919

#### Before:
```c
cg_emit_all_scope_cleanup(cg);
LLVMBuildRetVoid(cg->builder);
```

#### After:
```c
cg->propagated_err_val = NULL;  /* no error value to capture in void return */
cg_emit_all_scope_cleanup_pentit(cg, 1);  /* N1: still an error path */
LLVMBuildRetVoid(cg->builder);
```

**Pentit value:** Always N1 (1). Even though the return is void, this IS the error path — the function is bailing out because a `?` check failed.

---

### Exit Point 5: Scope exit — block closing brace (cg_stmt.c:20)

**Context:** `cg_emit_scope_cleanup` — called when a `{ }` block exits normally

**File:** `src/cg_stmt.c`
**Line:** 20

#### Before:
```c
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_defers(cg, scope);
    cg_emit_scope_drops(cg, scope);
}
```

#### After:
(Already done in Part 1 — wrapper passes Z)
```c
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_cleanup_pentit(cg, scope, 2);  /* Z: normal scope exit */
}
```

**Pentit value:** Always Z (2). A block ending is a normal scope exit. Not an error.

---

### Exit Point 6: break/continue (cg_stmt.c:34)

**Context:** `cg_emit_cleanup_to_scope` — loop exit via break or continue

**File:** `src/cg_stmt.c`
**Line:** 34

#### Before/After:
(Already done in Part 1)
```c
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_cleanup_pentit(cg, s, 2);  /* Z: loop exit is not an error */
    }
}
```

**Pentit value:** Always Z (2). Breaking out of a loop is not an error condition.

---

### Exit Point 7: panic (NEW)

**Context:** When a panic is triggered (assertion failure, unreachable code, explicit `panic()` call), the exit path is N2 — the most extreme negative.

**File:** `src/cg_stmt.c` (or `src/cg_builtin.c` if panic is a builtin)

#### After:
```c
/* Panic path: N2 — only panicdefer and errdefer (N-side) fire.
 * defer (Z) also fires because Z fires always.
 * successdefer and recoverdefer do NOT fire. */
cg_emit_all_scope_cleanup_pentit(cg, 0);  /* N2: panic */
/* Emit abort/trap after cleanup */
```

**Pentit value:** Always N2 (0). Panic is the most extreme negative exit. This triggers:
- `panicdefer` (trigger=0, N2): fires because `0 <= 0`
- `errdefer` (trigger=1, N1): fires because `0 <= 1`
- `defer` (trigger=2, Z): fires because Z always fires
- `successdefer` (trigger=3, P1): does NOT fire because `0 >= 3` is false
- `recoverdefer` (trigger=4, P2): does NOT fire because `0 >= 4` is false

---

## Runtime path detection: `return result_variable`

When the return expression is a bare variable (not `ok(...)` or `err(...)`), and its type is Result, we can't know at compile time whether it's an ok or err. We need a runtime branch.

**File:** `src/cg_stmt.c`
**New function added after `cg_emit_cleanup_to_scope`:**

```c
/* Runtime Result exit path detection.
 * Generates:
 *   %is_ok = extractvalue %result, 0
 *   br %is_ok, ok_cleanup_bb, err_cleanup_bb
 *
 *   ok_cleanup_bb:
 *     ; Z (normal) cleanup: defers only
 *     cg_emit_all_scope_cleanup_pentit(cg, 2)
 *     br merge_bb
 *
 *   err_cleanup_bb:
 *     ; N1 (error) cleanup: errdefers + defers
 *     cg_emit_all_scope_cleanup_pentit(cg, 1)
 *     br merge_bb
 *
 *   merge_bb:
 *     ; continue to actual return
 */
static void cg_emit_result_path_branch(Codegen *cg, LLVMValueRef result_val) {
    LLVMValueRef is_ok = LLVMBuildExtractValue(cg->builder, result_val, 0, "exit.is_ok");

    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.ok.cleanup");
    LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.err.cleanup");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.after.cleanup");

    LLVMBuildCondBr(cg->builder, is_ok, ok_bb, err_bb);

    /* OK path: Z cleanup only */
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    cg_emit_all_scope_cleanup_pentit(cg, 2);   /* Z (2) */
    if (!cg_block_terminated(cg))
        LLVMBuildBr(cg->builder, merge_bb);

    /* Error path: N1 cleanup (errdefers + defers) */
    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    /* Extract error value for errdefer |e| capture */
    cg->propagated_err_val = LLVMBuildExtractValue(cg->builder,
        result_val, 1, "exit.err.data");
    cg_emit_all_scope_cleanup_pentit(cg, 1);  /* N1 (1) */
    cg->propagated_err_val = NULL;
    if (!cg_block_terminated(cg))
        LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}
```

**When this triggers:** Only for `return someVariable` where `someVariable` is typed as Result. For `return ok(...)` and `return err(...)`, the path is known at compile time and no branch is needed.

**Note:** The runtime branch only distinguishes Z vs N1. It does not produce N2 (panic), P1 (success), or P2 (recovery) — those paths are always statically known at compile time. Panic is explicit. Recovery comes from the restart mechanism. The runtime ambiguity is only between "was this Result an ok or an err?"

---

## Summary: exit point → pentit mapping

| Exit point | File:Line | Pentit | Value | Rationale |
|-----------|-----------|--------|-------|-----------|
| `return ok(...)` | cg_stmt.c:345 | Z | 2 | Explicit success |
| `return err(...)` | cg_stmt.c:345 | N1 | 1 | Explicit error |
| `return result_var` | cg_stmt.c:345 | Runtime | 1 or 2 | Can't know statically |
| `return void` | cg_stmt.c:345 | Z | 2 | Normal exit |
| TCO tail call | cg_stmt.c:316 | Z | 2 | Continuation, not error |
| `?` error branch | cg_literal.c:915 | N1 | 1 | Error propagation |
| `?` void fallback | cg_literal.c:919 | N1 | 1 | Error propagation |
| Scope exit `}` | cg_stmt.c:20 | Z | 2 | Normal scope end |
| break/continue | cg_stmt.c:34 | Z | 2 | Loop control, not error |
| panic | cg_stmt.c (or builtin) | N2 | 0 | Fatal/unrecoverable |
| Restart recovery | (Part 4) | P2 | 4 | Error was fixed |

---

## Verification

After this step: run all 772 Furling Gate tests. Since no existing code uses errdefer (trigger=1), panicdefer (trigger=0), successdefer (trigger=3), or recoverdefer (trigger=4), the behavior is identical. The only change is that the `?` error path now sets `propagated_err_val` before cleanup (which is a no-op since no errdefers with capture exist yet).
