# Part 2: Exit Path Detection

**Goal:** Map every exit point in the C bootstrap codegen and assign it a trit value. No new keywords, no new syntax — just change the existing `cg_emit_all_scope_cleanup(cg)` calls to `cg_emit_all_scope_cleanup_trit(cg, exit_path)` with the correct trit.

**Files modified:** `cg_stmt.c`, `cg_literal.c`

**Depends on:** Part 1 (DeferEntry struct and _trit functions exist)

---

## Complete exit point inventory

There are exactly 6 places in the C bootstrap that call cleanup functions before exiting a scope or function. Each one gets a trit.

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
/* Determine exit path trit from the return expression */
int8_t exit_path = 0;  /* default: Z (normal) */
if (node->u.single.expr) {
    AstNode *ret_expr = node->u.single.expr;
    if (ret_expr->kind == ND_ERR) {
        /* return err(...) — explicit error return */
        exit_path = -1;  /* N */
    } else if (ret_expr->kind == ND_OK) {
        /* return ok(...) — explicit ok return */
        exit_path = 0;   /* Z */
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
cg_emit_all_scope_cleanup_trit(cg, exit_path);
if (val) {
    LLVMTypeRef fn_ret_type = LLVMGetReturnType(
        LLVMGlobalGetValueType(cg->cur_fn));
    val = cg_coerce_int(cg, val, fn_ret_type);
    LLVMBuildRet(cg->builder, val);
} else
    LLVMBuildRetVoid(cg->builder);
```

**Trit value:**
- `return ok(...)` → Z (0)
- `return err(...)` → N (-1)
- `return void` → Z (0)
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
cg_emit_all_scope_cleanup_trit(cg, 0);  /* Z: tail calls succeed */
LLVMBuildBr(cg->builder, cg->tco_loop_bb);
```

**Trit value:** Always Z (0). A tail call is a successful continuation. If the tail call returns an error, the NEXT iteration handles it — this iteration succeeded in dispatching.

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
cg_emit_all_scope_cleanup_trit(cg, -1);  /* N: error propagation */
cg->propagated_err_val = NULL;  /* clear after use */
LLVMBuildRet(cg->builder, ret_val);
```

**Trit value:** Always N (-1). The `?` operator error branch IS the error path by definition. This is the most important call site — it's where errdefer actually fires.

**The `propagated_err_val` assignment** is critical: it stores the error value so that `cg_emit_scope_defers_trit` can bind it for `errdefer |e| { ... }` capture blocks.

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
cg_emit_all_scope_cleanup_trit(cg, -1);  /* N: still an error path */
LLVMBuildRetVoid(cg->builder);
```

**Trit value:** Always N (-1). Even though the return is void, this IS the error path — the function is bailing out because a `?` check failed.

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
    cg_emit_scope_cleanup_trit(cg, scope, 0);  /* Z: normal scope exit */
}
```

**Trit value:** Always Z (0). A block ending is a normal scope exit. Not an error.

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
        cg_emit_scope_cleanup_trit(cg, s, 0);  /* Z: loop exit is not an error */
    }
}
```

**Trit value:** Always Z (0). Breaking out of a loop is not an error condition.

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
 *     cg_emit_all_scope_cleanup_trit(cg, 0)
 *     br merge_bb
 *
 *   err_cleanup_bb:
 *     ; N (error) cleanup: errdefers + defers
 *     cg_emit_all_scope_cleanup_trit(cg, -1)
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
    cg_emit_all_scope_cleanup_trit(cg, 0);   /* Z */
    if (!cg_block_terminated(cg))
        LLVMBuildBr(cg->builder, merge_bb);

    /* Error path: N cleanup (errdefers + defers) */
    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    /* Extract error value for errdefer |e| capture */
    cg->propagated_err_val = LLVMBuildExtractValue(cg->builder,
        result_val, 1, "exit.err.data");
    cg_emit_all_scope_cleanup_trit(cg, -1);  /* N */
    cg->propagated_err_val = NULL;
    if (!cg_block_terminated(cg))
        LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}
```

**When this triggers:** Only for `return someVariable` where `someVariable` is typed as Result. For `return ok(...)` and `return err(...)`, the path is known at compile time and no branch is needed.

---

## Summary: exit point → trit mapping

| Exit point | File:Line | Trit | Rationale |
|-----------|-----------|------|-----------|
| `return ok(...)` | cg_stmt.c:345 | Z (0) | Explicit success |
| `return err(...)` | cg_stmt.c:345 | N (-1) | Explicit error |
| `return result_var` | cg_stmt.c:345 | Runtime branch | Can't know statically |
| `return void` | cg_stmt.c:345 | Z (0) | Normal exit |
| TCO tail call | cg_stmt.c:316 | Z (0) | Continuation, not error |
| `?` error branch | cg_literal.c:915 | N (-1) | Error propagation |
| `?` void fallback | cg_literal.c:919 | N (-1) | Error propagation |
| Scope exit `}` | cg_stmt.c:20 | Z (0) | Normal scope end |
| break/continue | cg_stmt.c:34 | Z (0) | Loop control, not error |
| Restart recovery | (Part 4) | P (+1) | Error was fixed |

---

## Verification

After this step: run all 768 Furling Gate tests. Since no existing code uses errdefer (trigger=-1) or recovery-defer (trigger=+1), the behavior is identical. The only change is that the `?` error path now sets `propagated_err_val` before cleanup (which is a no-op since no errdefers with capture exist yet).
