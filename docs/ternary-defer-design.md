# Ternary Defer System Design

**Date:** March 24, 2026
**Status:** Design — no code changes yet
**Depends on:** trit/tryte primitives (already in C bootstrap)

---

## The Insight

Error handling paths aren't binary. They're ternary:

| Path | Meaning | Cleanup behavior |
|------|---------|-----------------|
| **N (-1)** | Error — operation failed, unwinding | Run errdefers + defers |
| **Z (0)** | Normal — operation succeeded | Run defers only |
| **P (+1)** | Recovered — error occurred but was handled via restart | Don't unwind, don't run errdefers |

This maps directly to balanced ternary. The same three-valued logic that drives trit operators, Cottrell Confluence, and Flow Ownership typestates now drives error handling.

---

## Current State: C Bootstrap Defer

### Data structure (codegen.h)

```c
// CURRENT — single array of raw expression pointers
struct CgScope {
    // ...
    AstNode **defers;
    int defer_count;
    int defer_cap;
};
```

### Push (cg_stmt.c:41)

```c
// CURRENT — all defers are equal
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

### Emit (cg_stmt.c:12)

```c
// CURRENT — runs all defers unconditionally in LIFO order
void cg_emit_scope_defers(Codegen *cg, CgScope *scope) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        cg_expr(cg, scope->defers[i]);
    }
}
```

### Call sites that trigger defer emission

| Location | File:Line | Exit path | Currently calls |
|----------|-----------|-----------|-----------------|
| Normal return | cg_stmt.c:345 | `return val;` | `cg_emit_all_scope_cleanup(cg)` |
| TCO return | cg_stmt.c:316 | tail call optimization | `cg_emit_all_scope_cleanup(cg)` |
| `?` error branch | cg_literal.c:915 | Result error propagation | `cg_emit_all_scope_cleanup(cg)` |
| `?` void fallback | cg_literal.c:919 | void error propagation | `cg_emit_all_scope_cleanup(cg)` |
| Scope exit | cg_stmt.c:20 | block `}` | `cg_emit_scope_cleanup(cg, scope)` |
| Break/continue | cg_stmt.c:34 | loop exit | `cg_emit_cleanup_to_scope(cg, stop)` |

Every call site currently runs ALL defers. None of them know whether they're on an error path.

---

## New Design: Ternary Defer

### Data structure change (codegen.h)

```c
// NEW — each entry tagged with a trit
typedef struct {
    AstNode  *expr;         // the deferred expression
    int8_t    trigger;      // -1=errdefer(N), 0=defer(Z), +1=reserved(P)
    AstNode  *err_capture;  // for errdefer |e| { ... } — NULL if no capture
} DeferEntry;

struct CgScope {
    // ...
    DeferEntry *defers;     // was AstNode **defers
    int defer_count;
    int defer_cap;
};
```

One array. One struct. The `trigger` field is a trit stored as i8. The `err_capture` field is for the error value capture syntax (`errdefer |e| { ... }`), NULL when not used.

### Push (cg_stmt.c — replaces cg_push_defer)

```c
// NEW — push with trit trigger
void cg_push_defer_entry(Codegen *cg, AstNode *expr, int8_t trigger, AstNode *err_capture) {
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

// Convenience wrappers
void cg_push_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 0, NULL);   // Z: always runs
}

void cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture) {
    cg_push_defer_entry(cg, expr, -1, err_capture);  // N: error path only
}
```

### Emit (cg_stmt.c — replaces cg_emit_scope_defers)

```c
// NEW — takes exit path trit, filters entries
//   exit_path = -1 (N): error exit → run errdefers (N) and defers (Z)
//   exit_path =  0 (Z): normal exit → run defers (Z) only
//   exit_path = +1 (P): recovered → run nothing (future: recovery-specific cleanup)
void cg_emit_scope_defers(Codegen *cg, CgScope *scope, int8_t exit_path) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        DeferEntry *d = &scope->defers[i];

        bool should_run = false;
        if (d->trigger == 0) {
            // Z (defer): runs on N (error) and Z (normal), not on P (recovered)
            should_run = (exit_path <= 0);
        } else if (d->trigger == -1) {
            // N (errdefer): runs only on N (error)
            should_run = (exit_path == -1);
        } else if (d->trigger == 1) {
            // P (future: recovery defer): runs only on P (recovered)
            should_run = (exit_path == 1);
        }

        if (should_run) {
            // If errdefer has error capture, bind the error value first
            if (d->trigger == -1 && d->err_capture && cg->propagated_err_val) {
                cg_bind_err_capture(cg, d->err_capture, cg->propagated_err_val);
            }
            cg_expr(cg, d->expr);
        }
    }
}
```

### Ternary truth table for "should this defer run?"

Using Cottrell Confluence logic — the trigger trit and exit path trit interact:

| Entry trigger | Exit N (error) | Exit Z (normal) | Exit P (recovered) |
|---------------|---------------|-----------------|---------------------|
| N (errdefer) | **RUN** | skip | skip |
| Z (defer) | **RUN** | **RUN** | skip |
| P (recovery) | skip | skip | **RUN** |

This is the **consensus** operation from Cottrell Confluence:
- N triggers activate on N exits
- Z triggers activate on N and Z exits (defer always runs except recovery)
- P triggers activate on P exits

The relationship: `should_run = (trigger <= exit_path)` when exit_path is N, `(trigger == 0)` when Z, `(trigger == exit_path)` when P. Or more precisely: a defer runs when its trigger's "concern" overlaps with the exit path.

### Updated call sites

```c
// cg_stmt.c — ND_RETURN handler
case ND_RETURN: {
    // ... evaluate return value ...

    // Determine exit path trit
    int8_t exit_path = 0;  // default: normal (Z)
    if (node->u.single.expr) {
        // Check if returning err(...) explicitly
        if (node->u.single.expr->kind == ND_ERR) {
            exit_path = -1;  // N: error return
        }
        // Check if returning a Result — need runtime check
        Type *ret_t = type_resolve(cg_expr_type(cg, node->u.single.expr));
        if (ret_t && ret_t->kind == TY_RESULT && exit_path == 0) {
            // Can't know statically if ok or err — emit runtime branch
            // (see "Runtime path detection" section below)
            exit_path = cg_emit_result_path_check(cg, val);
        }
    }

    cg_emit_all_scope_cleanup_with_path(cg, exit_path);
    // ... emit ret ...
}
```

```c
// cg_literal.c — ND_PROPAGATE (?) error branch
// This is ALREADY on the error path — we know it's N
case ND_PROPAGATE: {
    // ... build error result ...

    /* Error path: exit_path = N (-1) */
    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    cg->propagated_err_val = err_val;  // store for errdefer |e| capture
    cg_emit_all_scope_cleanup_with_path(cg, -1);  // N: error
    LLVMBuildRet(cg->builder, ret_val);

    /* OK path: continues, no cleanup needed here */
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    // ...
}
```

```c
// cg_stmt.c — scope exit (block closing brace)
// Normal scope exit is always Z
void cg_emit_scope_cleanup(Codegen *cg, CgScope *scope) {
    cg_emit_scope_defers(cg, scope, 0);  // Z: normal
    cg_emit_scope_drops(cg, scope);
}
```

```c
// cg_stmt.c — break/continue
// Loop exit is always Z (not an error)
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_defers(cg, s, 0);  // Z: normal
        cg_emit_scope_drops(cg, s);
    }
}
```

### Runtime path detection for `return result_variable`

When the return value is a Result variable (not an explicit `ok(...)` or `err(...)`), we can't know the path at compile time. We need a runtime check:

```c
// Generate: if (result.is_ok) { run Z defers } else { run N defers }
int8_t cg_emit_result_path_check(Codegen *cg, LLVMValueRef result_val) {
    // Extract is_ok field
    LLVMValueRef is_ok = LLVMBuildExtractValue(cg->builder, result_val, 0, "exit.is_ok");

    LLVMBasicBlockRef ok_cleanup = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.ok.cleanup");
    LLVMBasicBlockRef err_cleanup = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.err.cleanup");
    LLVMBasicBlockRef after_cleanup = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.after.cleanup");

    LLVMBuildCondBr(cg->builder, is_ok, ok_cleanup, err_cleanup);

    // OK path: emit Z defers only
    LLVMPositionBuilderAtEnd(cg->builder, ok_cleanup);
    cg_emit_all_scope_defers_only(cg, 0);  // Z
    LLVMBuildBr(cg->builder, after_cleanup);

    // Error path: emit N + Z defers
    LLVMPositionBuilderAtEnd(cg->builder, err_cleanup);
    cg_emit_all_scope_defers_only(cg, -1);  // N
    LLVMBuildBr(cg->builder, after_cleanup);

    LLVMPositionBuilderAtEnd(cg->builder, after_cleanup);
    return 0;  // path already handled via branches
}
```

---

## Parser Changes

### New keyword

```c
// token.h — add after TOK_KW_DEFER
TOK_KW_ERRDEFER,
```

### Lexer keyword recognition

```c
// lexer.c — in keyword lookup
if (len == 8 && memcmp(start, "errdefer", 8) == 0) return TOK_KW_ERRDEFER;
```

### New AST node (or reuse ND_DEFER with flag)

Option A — new node kind:
```c
// ast.h
ND_ERRDEFER,    // errdefer expr; or errdefer |e| { expr }
```

Option B — flag on ND_DEFER (preferred — same node, different trigger):
```c
// ast.h — add to ND_DEFER's union
struct {
    AstNode *expr;
    bool is_errdefer;
    AstNode *err_capture;   // |e| binding, NULL if none
} defer;
```

Option B is cleaner — one node kind, one parser path, one codegen path. The `is_errdefer` flag maps to the trit trigger.

### Parser (parser.c — in parse_statement)

```c
// CURRENT
if (match(p, TOK_KW_DEFER)) {
    AstNode *n = make_node(p, ND_DEFER, loc);
    n->u.single.expr = parse_expression(p, 0);
    match(p, TOK_SEMICOLON);
    return n;
}

// NEW — handle both defer and errdefer
if (match(p, TOK_KW_DEFER) || match(p, TOK_KW_ERRDEFER)) {
    bool is_err = (p->prev.kind == TOK_KW_ERRDEFER);
    AstNode *n = make_node(p, ND_DEFER, loc);
    n->u.defer.is_errdefer = is_err;
    n->u.defer.err_capture = NULL;

    // errdefer |e| { ... } — capture error value
    if (is_err && check(p, TOK_PIPE)) {
        advance(p);  // |
        n->u.defer.err_capture = parse_ident_as_param(p);
        expect(p, TOK_PIPE, "'|'");
    }

    n->u.defer.expr = parse_expression(p, 0);
    match(p, TOK_SEMICOLON);
    return n;
}
```

### Codegen (cg_stmt.c — in cg_stmt switch)

```c
// CURRENT
case ND_DEFER:
    if (node->u.single.expr)
        cg_push_defer(cg, node->u.single.expr);
    break;

// NEW
case ND_DEFER:
    if (node->u.defer.expr) {
        if (node->u.defer.is_errdefer)
            cg_push_errdefer(cg, node->u.defer.expr, node->u.defer.err_capture);
        else
            cg_push_defer(cg, node->u.defer.expr);
    }
    break;
```

---

## ShelbyC Syntax

```
// defer — always runs (Z)
fd := Open(path)?;
defer Close(fd);

// errdefer — runs only on error path (N)
buf := Alloc(1024)?;
errdefer Free(buf);

// errdefer with capture — access the error value
conn := Connect(host)?;
errdefer |e| {
    Log("connection failed: ", e);
    Cleanup(conn);
}

// Combined pattern — the full resource acquisition idiom
fn Setup(path str) Result<Connection, Error> {
    fd := Open(path)?;
    errdefer Close(fd);              // only if we fail below

    buf := Alloc(4096)?;
    errdefer Free(buf);              // only if we fail below

    conn := Handshake(fd, buf)?;
    errdefer |e| Log("handshake failed", e);

    Validate(conn)?;

    return ok(Connection{ fd: fd, buf: buf, conn: conn });
    // success: no errdefers run, caller owns everything
}
```

---

## Future: Recovery Path (P)

When conditions and restarts are added, the P (+1) path becomes:

```
fn ReadConfig(path str) Result<Config, Error> {
    data := ReadFile(path)
        restart RetryWith(alt str) { return ReadFile(alt); }
        restart UseDefault() { return "{}"; };

    errdefer Log("config parse failed");
    defer CleanupTempFiles();

    config := Parse(data)?;
    return ok(config);
}

// Caller chooses recovery
handle ReadConfig("config.json") {
    on FileNotFound => invoke RetryWith("/etc/default.conf")
    on ParseError => invoke UseDefault()
}
```

When a restart is invoked:
- Exit path = P (+1)
- Errdefers do NOT run (error was recovered)
- Normal defers still run at scope exit (not at restart point)
- P-trigger defers (future) run for recovery-specific cleanup

The DeferEntry struct already has the `trigger` field ready. Adding restarts means:
1. New AST nodes for `restart` declarations and `handle`/`invoke` blocks
2. The restart mechanism (longjmp-like or continuation-based) that resumes at the restart point
3. The exit path trit is set to P when a restart succeeds

The ternary defer stack doesn't change. The emit function doesn't change. Only the set of possible exit_path values expands from {N, Z} to {N, Z, P}.

---

## Implementation Order

1. **Add `DeferEntry` struct** to codegen.h — replace `AstNode **defers` with `DeferEntry *defers`
2. **Update `cg_push_defer`** to use DeferEntry with trigger=0
3. **Update `cg_emit_scope_defers`** to take exit_path parameter
4. **Update all call sites** to pass the correct exit_path trit
5. **Verify** all 768 Furling Gate tests still pass (no behavior change — all defers are trigger=0)
6. **Add `TOK_KW_ERRDEFER`** to lexer
7. **Add `errdefer` parsing** with |e| capture support
8. **Add `cg_push_errdefer`** (trigger=-1)
9. **Write tests** for errdefer: basic, with capture, mixed with defer, nested scopes
10. **Add runtime path detection** for `return result_variable`
11. **Run full Furling Gate** — 768/768 + new errdefer tests

Steps 1-5 are pure refactor — same behavior, new structure. Steps 6-9 add the feature. Steps 10-11 handle the edge case and prove it.

---

## Connection to the Language

| ShelbyC concept | Ternary parallel |
|----------------|-----------------|
| trit values | N(-1), Z(0), P(+1) |
| Cottrell Confluence | operator resolution via trit tables |
| Flow Ownership typestate | owned(Z), borrowed(N), moved(P) |
| Defer trigger | errdefer(N), defer(Z), recovery(P) |
| Error path | fail(N), succeed(Z), recover(P) |

Same three-valued logic at every level. The language is consistent from bits to error handling.

---

*errdefer is not "borrowed from Zig." It's the N trit of a ternary cleanup system that Zig doesn't have. The balanced ternary isn't just for math — it's the language's type system, control flow, and error handling model.*
