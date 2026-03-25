# The Ternary Error System: Defer, Errdefer, Conditions, and Restarts

**Date:** March 24, 2026
**Status:** Complete design — no code changes yet
**Prerequisite reading:** coupled-ownership-solver.md, self-hosting-semantics.md, tensor-type-system.md

---

## The Core Principle

ShelbyC uses balanced ternary at every level:

| Level | N (-1) | Z (0) | P (+1) |
|-------|--------|-------|--------|
| **trit values** | negative | zero | positive |
| **Cottrell operators** | `& (min)` | `+ (mod-3)` | `\| (max)` |
| **Ownership typestate** | moved/consumed | borrowed | owned/alive |
| **Error handling** | error — unwind | normal — succeed | recovered — restart |

This document designs the error handling row — the ternary defer system with conditions and restarts. All three paths. No "future work."

---

## Part 1: The Defer Stack

### Current C bootstrap implementation

**codegen.h — CgScope struct:**
```c
// CURRENT: flat array of expression pointers
struct CgScope {
    CgLocal  **buckets;
    int        bucket_count;
    CgScope   *parent;

    struct DropEntry {
        LLVMValueRef alloca;
        Type        *ty;
        LLVMValueRef drop_flag;
    } *drops;
    int drop_count;
    int drop_cap;

    AstNode **defers;      // <-- this changes
    int defer_count;
    int defer_cap;
};
```

**cg_stmt.c — current defer push:**
```c
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

**cg_stmt.c — current defer emit:**
```c
void cg_emit_scope_defers(Codegen *cg, CgScope *scope) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        cg_expr(cg, scope->defers[i]);
    }
}
```

### New design: DeferEntry with trit trigger

**codegen.h — new struct:**
```c
typedef struct {
    AstNode  *expr;         /* the deferred expression or block */
    int8_t    trigger;      /* N(-1)=errdefer, Z(0)=defer, P(+1)=recovery-defer */
    AstNode  *err_capture;  /* for errdefer |e| { ... } — NULL if no capture */
} DeferEntry;

struct CgScope {
    CgLocal  **buckets;
    int        bucket_count;
    CgScope   *parent;

    struct DropEntry {
        LLVMValueRef alloca;
        Type        *ty;
        LLVMValueRef drop_flag;
    } *drops;
    int drop_count;
    int drop_cap;

    DeferEntry *defers;     /* unified stack: defer + errdefer + recovery-defer */
    int defer_count;
    int defer_cap;
};
```

**cg_stmt.c — new push:**
```c
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

/* Convenience wrappers matching the three trit values */
void cg_push_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 0, NULL);    /* Z: always runs */
}
void cg_push_errdefer(Codegen *cg, AstNode *expr, AstNode *err_capture) {
    cg_push_defer_entry(cg, expr, -1, err_capture);  /* N: error path only */
}
void cg_push_recovery_defer(Codegen *cg, AstNode *expr) {
    cg_push_defer_entry(cg, expr, 1, NULL);    /* P: recovery path only */
}
```

**cg_stmt.c — new emit with exit path trit:**
```c
/*
 * Emit defers filtered by exit path trit.
 *
 * Truth table (Cottrell Confluence logic):
 *
 *   Entry trigger | Exit N (error) | Exit Z (normal) | Exit P (recovered)
 *   --------------|----------------|-----------------|--------------------
 *   N (errdefer)  | RUN            | skip            | skip
 *   Z (defer)     | RUN            | RUN             | skip
 *   P (recovery)  | skip           | skip            | RUN
 *
 * Z (defer) runs on both N and Z because cleanup is needed whether you
 * succeed or fail. It does NOT run on P because recovery means the
 * operation was retried and succeeded — resources are still needed.
 *
 * N (errdefer) runs only on N because error-specific cleanup is only
 * meaningful when actually unwinding from an error.
 *
 * P (recovery-defer) runs only on P for recovery-specific teardown
 * (e.g., closing a fallback resource that was opened during restart).
 */
void cg_emit_scope_defers_trit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    for (int i = scope->defer_count - 1; i >= 0; i--) {
        if (cg_block_terminated(cg)) break;
        DeferEntry *d = &scope->defers[i];

        bool should_run;
        switch (d->trigger) {
        case  0: should_run = (exit_path <= 0); break;  /* Z: runs on N and Z */
        case -1: should_run = (exit_path == -1); break;  /* N: runs on N only */
        case  1: should_run = (exit_path == 1); break;   /* P: runs on P only */
        default: should_run = false; break;
        }

        if (!should_run) continue;

        /* If errdefer with capture, bind the error value before executing */
        if (d->trigger == -1 && d->err_capture && cg->propagated_err_val) {
            /* Create local binding for the captured error */
            LLVMTypeRef err_ty = LLVMTypeOf(cg->propagated_err_val);
            LLVMValueRef err_alloca = cg_entry_alloca(cg, err_ty, "errdefer.capture");
            LLVMBuildStore(cg->builder, cg->propagated_err_val, err_alloca);
            cg_register_local(cg, d->err_capture->u.ident.name,
                             d->err_capture->u.ident.name_len,
                             err_alloca, NULL, -1);
        }

        cg_expr(cg, d->expr);
    }
}
```

**Updated cleanup functions:**
```c
/* scope cleanup with exit path */
void cg_emit_scope_cleanup_trit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    cg_emit_scope_defers_trit(cg, scope, exit_path);
    cg_emit_scope_drops(cg, scope);
}

/* all scopes cleanup with exit path */
void cg_emit_all_scope_cleanup_trit(Codegen *cg, int8_t exit_path) {
    for (CgScope *s = cg->scope; s; s = s->parent) {
        cg_emit_scope_cleanup_trit(cg, s, exit_path);
        if (s == cg->fn_scope) break;
    }
}

/* break/continue cleanup — always Z (not an error) */
void cg_emit_cleanup_to_scope(Codegen *cg, CgScope *stop) {
    for (CgScope *s = cg->scope; s && s != stop; s = s->parent) {
        cg_emit_scope_cleanup_trit(cg, s, 0);  /* Z: normal exit */
    }
}
```

---

## Part 2: Exit Path Detection

### Every exit point in the C bootstrap, with its trit value

**1. Normal return (cg_stmt.c:287) — Z or N depending on return value:**
```c
case ND_RETURN: {
    /* ... evaluate return value ... */
    LLVMValueRef val = cg_expr(cg, node->u.single.expr);

    /* Determine exit path trit */
    int8_t exit_path = 0;  /* default: Z (normal) */
    if (node->u.single.expr) {
        /* Explicit err(...) → N */
        if (node->u.single.expr->kind == ND_ERR) {
            exit_path = -1;
        }
        /* Explicit ok(...) → Z */
        else if (node->u.single.expr->kind == ND_OK) {
            exit_path = 0;
        }
        /* Returning a Result variable → runtime check */
        else {
            Type *ret_t = type_resolve(cg_expr_type(cg, node->u.single.expr));
            if (ret_t && ret_t->kind == TY_RESULT) {
                exit_path = -2;  /* sentinel: needs runtime branch */
            }
        }
    }

    if (exit_path == -2) {
        /* Runtime branch: check is_ok to determine path */
        cg_emit_result_path_branch(cg, val);
    } else {
        cg_emit_all_scope_cleanup_trit(cg, exit_path);
    }

    /* ... emit ret ... */
}
```

**2. ? propagation error branch (cg_literal.c:869) — always N:**
```c
case ND_PROPAGATE: {
    /* ... check is_ok, branch ... */

    /* Error path — this IS the N path */
    LLVMPositionBuilderAtEnd(cg->builder, err_bb);

    /* Store error value for errdefer |e| capture */
    cg->propagated_err_val = err_val;

    /* Build return Result with is_ok=false ... */
    cg_emit_all_scope_cleanup_trit(cg, -1);  /* N: error */
    LLVMBuildRet(cg->builder, ret_val);

    /* OK path — continues normally, no cleanup here */
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    /* ... extract ok value ... */
}
```

**3. Scope exit / block closing brace (cg_stmt.c:20) — always Z:**
```c
void cg_emit_scope_cleanup_trit(Codegen *cg, CgScope *scope, int8_t exit_path) {
    cg_emit_scope_defers_trit(cg, scope, 0);  /* Z at scope exit */
    cg_emit_scope_drops(cg, scope);
}
```

**4. break/continue (cg_stmt.c:34) — always Z:**
```c
/* Loop exit is not an error */
cg_emit_cleanup_to_scope(cg, stop);  /* internally uses Z */
```

**5. TCO tail call (cg_stmt.c:316) — always Z:**
```c
/* Tail call optimization: cleanup before jumping to loop header */
cg_emit_all_scope_cleanup_trit(cg, 0);  /* Z: tail calls aren't errors */
```

**6. Restart recovery (NEW) — P:**
```c
/* After a restart handler runs and returns a replacement value */
cg->restart_succeeded = true;
cg_emit_all_scope_cleanup_trit(cg, 1);  /* P: recovered */
/* Continue from restart point with replacement value */
```

### Runtime path detection for `return result_variable`

When we can't determine the path at compile time:

```c
/*
 * Generate:
 *   if (result.is_ok) {
 *       // Z path cleanup
 *   } else {
 *       // N path cleanup (includes errdefers)
 *   }
 *   return result;
 */
void cg_emit_result_path_branch(Codegen *cg, LLVMValueRef result_val) {
    LLVMValueRef is_ok = LLVMBuildExtractValue(cg->builder, result_val, 0, "exit.is_ok");

    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.ok.cleanup");
    LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.err.cleanup");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
        cg->ctx, cg->cur_fn, "exit.merge");

    LLVMBuildCondBr(cg->builder, is_ok, ok_bb, err_bb);

    /* OK path: Z cleanup */
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    cg_emit_all_scope_cleanup_trit(cg, 0);
    LLVMBuildBr(cg->builder, merge_bb);

    /* Error path: N cleanup (errdefers + defers) */
    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    cg->propagated_err_val = LLVMBuildExtractValue(cg->builder, result_val, 1, "exit.err.val");
    cg_emit_all_scope_cleanup_trit(cg, -1);
    LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}
```

---

## Part 3: Conditions and Restarts

### The concept

When an error occurs, the stack doesn't unwind immediately. Instead:

1. **Condition signaled**: the code at the error site declares "this went wrong"
2. **Restart options**: the code near the error defines "here's what I CAN do about it"
3. **Handler chooses**: the code up the call chain says "do that one"
4. **Execution resumes**: the restart runs, producing a replacement value. The function continues from the restart point as if the error never happened.

This is NOT exceptions. The stack doesn't unwind. The restart runs at the point of the error, not at the handler's location. The handler just chooses which restart to invoke.

### Syntax

```
fn ReadConfig(path str) Result<Config, Error> {
    // Restart declarations: available recovery options
    data := ReadFile(path)
        restart RetryWith(alt str) { return ReadFile(alt); }
        restart UseDefault() { return "{}"; };

    errdefer |e| Log("config failed: ", e);
    defer CleanupTemp();

    config := Parse(data)?;
    return ok(config);
}
```

```
// Caller chooses recovery policy
fn LoadConfig() Config {
    result := handle ReadConfig("config.json") {
        on FileNotFound => invoke RetryWith("/etc/default.conf")
        on ParseError   => invoke UseDefault()
    };
    return result;
}
```

### How it maps to the ternary system

```
                 ┌─────────────────────────────────────────────┐
                 │              ReadConfig("config.json")       │
                 │                                              │
                 │  data := ReadFile(path)                      │
                 │    restart RetryWith(alt) { ReadFile(alt) }  │
                 │    restart UseDefault()  { "{}" }            │
                 │                                              │
                 │  errdefer |e| Log("failed:", e)              │
                 │  defer CleanupTemp()                         │
                 │                                              │
                 │  config := Parse(data)?                      │
                 │  return ok(config)                           │
                 └──────┬──────────┬───────────┬───────────────┘
                        │          │           │
                   ReadFile OK   ReadFile FAIL   Parse FAIL
                        │          │           │
                        ▼          ▼           ▼
                    ┌───────┐  ┌─────────┐  ┌─────────┐
                    │ Z (0) │  │ handler │  │ N (-1)  │
                    │normal │  │ checks  │  │ error   │
                    │ path  │  │restarts │  │ unwind  │
                    └───┬───┘  └────┬────┘  └────┬────┘
                        │          │             │
                        │     ┌────┴────┐        │
                        │     │ restart │        │
                        │     │invoked? │        │
                        │     └──┬───┬──┘        │
                        │     yes│   │no         │
                        │        │   │           │
                        │        ▼   ▼           │
                        │   ┌───────┐ ┌───────┐  │
                        │   │ P (+1)│ │ N (-1)│  │
                        │   │recover│ │ error │  │
                        │   │ path  │ │unwind │  │
                        │   └───┬───┘ └───┬───┘  │
                        │       │         │      │
                        ▼       ▼         ▼      ▼
                   ┌─────────────────────────────────┐
                   │     Defer stack emit             │
                   │                                  │
                   │  Z path: run defers only         │
                   │  N path: run errdefers + defers  │
                   │  P path: run recovery-defers     │
                   └─────────────────────────────────┘
```

### AST nodes

```c
/* token.h — new keywords */
TOK_KW_ERRDEFER,    /* errdefer */
TOK_KW_RESTART,     /* restart */
TOK_KW_HANDLE,      /* handle */
TOK_KW_INVOKE,      /* invoke */
TOK_KW_ON,          /* on (inside handle block) */

/* ast.h — new nodes */
ND_ERRDEFER,        /* errdefer expr; or errdefer |e| { block } */
ND_RESTART_DECL,    /* restart Name(params) { body } */
ND_HANDLE,          /* handle expr { on Error => invoke Restart(args) } */
ND_HANDLE_ARM,      /* on ErrorType => invoke RestartName(args) */
```

Or, following the principle from the ternary defer design — use ND_DEFER with the trigger flag:

```c
/* ast.h — ND_DEFER gains .is_errdefer and .err_capture */
/* ND_DEFER union: */
struct {
    AstNode *expr;
    bool     is_errdefer;    /* false=defer(Z), true=errdefer(N) */
    AstNode *err_capture;    /* |e| binding, NULL if none */
} defer;

/* New nodes for restart system: */
ND_RESTART_DECL,     /* attached to an expression: expr restart Name(p) { body } */
ND_HANDLE_BLOCK,     /* handle expr { arms } */
ND_HANDLE_ARM,       /* on ErrorKind => invoke RestartName(args) */
```

### Parser changes

**parser.c — errdefer (in parse_statement):**
```c
/* Errdefer: errdefer expr; or errdefer |e| { block } */
if (match(p, TOK_KW_ERRDEFER)) {
    AstNode *n = make_node(p, ND_DEFER, loc);
    n->u.defer.is_errdefer = true;
    n->u.defer.err_capture = NULL;

    /* Optional error capture: errdefer |e| { ... } */
    if (check(p, TOK_PIPE)) {
        advance(p);  /* | */
        SrcLoc cap_loc = p->cur.loc;
        expect_ident(p);
        n->u.defer.err_capture = make_ident_node(p, cap_loc);
        expect(p, TOK_PIPE, "'|'");
    }

    if (check(p, TOK_LBRACE))
        n->u.defer.expr = parse_block(p);
    else {
        n->u.defer.expr = parse_expression(p, 0);
        match(p, TOK_SEMICOLON);
    }
    return n;
}
```

**parser.c — restart declarations (in parse_expression, after primary):**
```c
/* Restart declarations chain after an expression:
 *   data := ReadFile(path)
 *       restart RetryWith(alt str) { return ReadFile(alt); }
 *       restart UseDefault() { return "{}"; };
 */
while (check(p, TOK_KW_RESTART)) {
    advance(p);  /* restart */
    SrcLoc rloc = p->cur.loc;
    expect_ident(p);
    AstNode *restart = make_node(p, ND_RESTART_DECL, rloc);
    restart->u.restart.name = p->prev.start;
    restart->u.restart.name_len = p->prev.len;

    /* Parameters */
    expect(p, TOK_LPAREN, "'('");
    restart->u.restart.params = parse_param_list(p);
    expect(p, TOK_RPAREN, "')'");

    /* Body */
    restart->u.restart.body = parse_block(p);

    /* Attach to the expression node */
    list_append(&expr_node->u.restartable.restarts, restart);
}
```

**parser.c — handle block (in parse_expression):**
```c
/* handle expr { on Type => invoke Restart(args), ... } */
if (match(p, TOK_KW_HANDLE)) {
    AstNode *n = make_node(p, ND_HANDLE_BLOCK, loc);
    n->u.handle.expr = parse_expression(p, 0);
    expect(p, TOK_LBRACE, "'{'");
    n->u.handle.arms = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        expect(p, TOK_KW_ON, "'on'");
        SrcLoc arm_loc = p->cur.loc;
        AstNode *arm = make_node(p, ND_HANDLE_ARM, arm_loc);

        /* Error type/pattern */
        arm->u.handle_arm.error_pattern = parse_pattern(p);
        expect(p, TOK_FAT_ARROW, "'=>'");

        /* invoke RestartName(args) */
        expect(p, TOK_KW_INVOKE, "'invoke'");
        expect_ident(p);
        arm->u.handle_arm.restart_name = p->prev.start;
        arm->u.handle_arm.restart_name_len = p->prev.len;

        if (check(p, TOK_LPAREN)) {
            advance(p);
            arm->u.handle_arm.args = parse_arg_list(p);
            expect(p, TOK_RPAREN, "')'");
        }

        list_append(&n->u.handle.arms, arm);
        match(p, TOK_COMMA);  /* optional trailing comma */
    }
    expect(p, TOK_RBRACE, "'}'");
    return n;
}
```

### Codegen for restarts

The restart mechanism uses continuation-passing internally. When an expression has restart declarations, the codegen:

1. Saves the current builder position (the restart point)
2. Creates a basic block for each restart
3. Creates a "restart table" — an array of function pointers, one per restart
4. On error, before unwinding, checks if a handle block registered restart choices
5. If yes, jumps to the chosen restart's basic block
6. The restart block executes its body and branches back to the instruction AFTER the original expression
7. Exit path = P (+1)

**cg_literal.c — expression with restarts:**
```c
/* Expression with restart declarations:
 *   data := ReadFile(path)
 *       restart RetryWith(alt) { return ReadFile(alt); }
 *       restart UseDefault()   { return "{}"; };
 *
 * Generates:
 *   %data = call ReadFile(path)
 *   %is_ok = extractvalue %data, 0
 *   br %is_ok, ok_bb, check_restarts_bb
 *
 *   check_restarts_bb:
 *     ; check if a handle block registered a choice
 *     %choice = load @restart_choice
 *     switch %choice, unwind_bb [
 *       0 → restart_retry_bb
 *       1 → restart_default_bb
 *     ]
 *
 *   restart_retry_bb:
 *     %alt = load @restart_arg_0
 *     %retried = call ReadFile(%alt)
 *     br after_bb          ; P path — recovered
 *
 *   restart_default_bb:
 *     %default = "{}"
 *     br after_bb          ; P path — recovered
 *
 *   unwind_bb:
 *     ; no restart chosen — normal N path
 *     emit errdefers + defers (exit_path = -1)
 *     ret err(...)
 *
 *   ok_bb:
 *     %unwrapped = extractvalue %data, 1
 *     br after_bb          ; Z path — normal
 *
 *   after_bb:
 *     %result = phi [%unwrapped, ok_bb], [%retried, restart_retry_bb],
 *                   [%default, restart_default_bb]
 *     ; continue with result
 */
```

### The handle block codegen

The handle block sets up the restart choice BEFORE calling the function:

```c
/* handle ReadConfig("config.json") {
 *     on FileNotFound => invoke RetryWith("/etc/default.conf")
 *     on ParseError   => invoke UseDefault()
 * }
 *
 * Generates:
 *   ; Store restart handler table for this call
 *   store @restart_choice_fn = &check_and_choose
 *   %result = call ReadConfig("config.json")
 *   ; Clear restart handler
 *   store @restart_choice_fn = null
 *
 * The check_and_choose function is emitted as:
 *   fn check_and_choose(err_val) -> (restart_index, args) {
 *       if err_val.Is(FileNotFound) return (0, "/etc/default.conf")
 *       if err_val.Is(ParseError)   return (1,)
 *       return (-1,)  // no restart matches — unwind normally
 *   }
 */
```

---

## Part 4: Connection to Flow Ownership

### Current typestate (flow.h)
```c
typedef enum {
    TS_ALIVE,           /* value exists and is usable */
    TS_CONSUMED,        /* value has been moved away */
    TS_MAYBE_CONSUMED,  /* consumed on some paths, alive on others */
    TS_DROPPED,         /* explicitly dropped */
} TypestateKind;
```

### Ternary typestate mapping

| TypestateKind | Trit | Meaning |
|---------------|------|---------|
| `TS_ALIVE` | P (+1) | owned, live, usable |
| `TS_BORROWED` (new) | Z (0) | temporarily lent, will return |
| `TS_CONSUMED` | N (-1) | moved away, gone |
| `TS_MAYBE_CONSUMED` | merge conflict | different trits on different paths |
| `TS_DROPPED` | N (-1) | same as consumed for flow purposes |

The merge function becomes a trit operation:
```c
/* CURRENT */
static TypestateKind typestate_merge(TypestateKind a, TypestateKind b) {
    if (a == b) return a;
    return TS_MAYBE_CONSUMED;
}

/* NEW — trit-aware merge */
static TypestateKind typestate_merge(TypestateKind a, TypestateKind b) {
    if (a == b) return a;
    /* Trit consensus: if both non-zero and different → conflict.
     * If one is Z (borrowed), the consensus is Z.
     * This matches the Cottrell ** operator semantics. */
    if (a == TS_BORROWED || b == TS_BORROWED) return TS_BORROWED;
    return TS_MAYBE_CONSUMED;
}
```

### How restarts interact with typestate

```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: TS_ALIVE (P)
    errdefer Free(buf);       // only on N path

    data := ReadFile(path)    // data: TS_ALIVE (P)
        restart UseCache() { return cache.Get(path); };

    // At this point, two paths converge:
    //   Z path: ReadFile succeeded → data is ALIVE, buf is ALIVE
    //   P path: restart succeeded  → data is ALIVE, buf is ALIVE
    //
    // The typestate merge: P merge P = P (both alive)
    // Errdefer did NOT run on P path (correct — buf still needed)

    Process(buf, data)?;      // uses both buf and data
    return ok(result);
}
```

The key insight: on the P (recovery) path, ownership state is PRESERVED. The restart produced a replacement value with the same type, so the variable is still TS_ALIVE. The errdefer didn't run, so the resources allocated before the restart point are still owned. The flow analysis doesn't need special handling for the P path — it's the same as the Z path from a typestate perspective.

### Coupled solver integration

From the coupled-ownership-solver blog post — L1 failure carries structured constraints:

```
{borrow: x@42, escape: closure_f@47, conflict: move_x@50}
```

With restarts, a new kind of constraint appears:

```
{restart_point: data@10, recovery: UseCache@12, exit_path: P}
```

L1 sees: "variable `data` was assigned at a restart point. If the restart fires, the value comes from UseCache instead of ReadFile. Both produce the same type. Typestate is ALIVE on all paths."

L1 doesn't need to escalate to L2 for restart points. The type system guarantees the replacement value has the same type. The ownership state is the same. The only thing the solver needs to verify is that the restart body doesn't violate ownership rules — which it checks like any other function body.

---

## Part 5: Connection to Tensor Tiles

From the tensor-type-system blog post — batch typestate propagation:

```
fn BatchPropagate<N, K>(
    states    [N]trit,
    effects   TritTile<N, K>,
) [N]trit {
    return effects.Infer(states);
}
```

The effects matrix for a function with defer/errdefer:

```
Variable: buf  data  conn  result
Effect 0: P    0     0     0      (Alloc: buf becomes alive)
Effect 1: 0    P     0     0      (ReadFile: data becomes alive)
Effect 2: 0    0     P     0      (Connect: conn becomes alive)
Effect 3: N    0     0     0      (errdefer Free(buf): buf consumed on N path)
Effect 4: 0    0     N     0      (errdefer Close(conn): conn consumed on N path)
```

The effects matrix IS a trit tile. Each row is a variable. Each column is an effect. The values are N/Z/P representing "consumed/unchanged/created."

Batch propagation of this matrix against a state vector gives the new typestate for all variables simultaneously. On the N path, errdefer effects (N values) activate. On the Z path, they don't. On the P path, recovery effects activate.

The same TBL + SADDLV instruction that does neural inference does ownership analysis. One codepath. One hardware instruction. Used by both the developer's program and the compiler's safety checker.

---

## Part 6: Implementation Order

All steps tested before proceeding to the next. All in the C bootstrap.

### Step 1: Refactor defer stack (no behavior change)
1. Replace `AstNode **defers` with `DeferEntry *defers` in CgScope
2. Update `cg_push_defer` to use DeferEntry with trigger=0
3. Update `cg_emit_scope_defers` to take exit_path but always pass Z=0
4. Update all call sites (5 locations documented above)
5. Run 768/768 Furling Gate — identical behavior

### Step 2: Add errdefer (N path)
6. Add `TOK_KW_ERRDEFER` to lexer
7. Add errdefer parsing with `|e|` capture support
8. Add `cg_push_errdefer` (trigger=-1)
9. Update `?` operator error branch to pass exit_path=-1
10. Update `ND_RETURN` with `ND_ERR` to pass exit_path=-1
11. Add `propagated_err_val` to Codegen struct for error capture
12. Write tests: basic errdefer, errdefer with capture, mixed defer+errdefer, nested scopes, errdefer ordering (LIFO)
13. Run Furling Gate + new tests

### Step 3: Add runtime path detection
14. Implement `cg_emit_result_path_branch` for `return result_variable`
15. Write tests: return ok, return err, return variable
16. Run Furling Gate + all tests

### Step 4: Add restarts (P path)
17. Add `TOK_KW_RESTART`, `TOK_KW_HANDLE`, `TOK_KW_INVOKE`, `TOK_KW_ON`
18. Add `ND_RESTART_DECL`, `ND_HANDLE_BLOCK`, `ND_HANDLE_ARM` to AST
19. Add restart parsing (declarations on expressions, handle blocks)
20. Add restart codegen: save point, restart basic blocks, switch on choice
21. Add handle codegen: set up restart choice function, call, clear
22. Add `cg_push_recovery_defer` (trigger=+1)
23. Write tests: basic restart, multiple restarts, handle with invoke, restart preserves ownership
24. Run Furling Gate + all tests

### Step 5: Integration with Flow Ownership
25. Add `TS_BORROWED` to TypestateKind (or map to existing)
26. Update `typestate_merge` with trit-aware merge
27. Verify flow analysis handles restart points (P path preserves typestate)
28. Write tests: errdefer + flow ownership, restart + flow ownership
29. Run Furling Gate + all tests

### Step 6: Full Furling Gate
30. Run ALL tests: 768 existing + all new defer/errdefer/restart tests
31. Verify no regressions
32. Update test tier documentation

---

## Part 7: ShelbyC Syntax Summary

```
// ---- Z (defer): always runs on scope exit ----
defer Close(fd);
defer { Cleanup(); Log("done"); }

// ---- N (errdefer): runs only when function exits via error ----
errdefer Free(buf);
errdefer |e| { Log("failed:", e); Rollback(); }
errdefer |e| match e {
    OutOfMemory => Panic("OOM");
    _ => Log(e);
}

// ---- Restart declarations: recovery options at the error site ----
data := ReadFile(path)
    restart RetryWith(alt str) { return ReadFile(alt); }
    restart UseDefault() { return "{}"; };

// ---- Handle block: caller chooses recovery ----
result := handle FetchData(url) {
    on Timeout    => invoke RetryWith(backup_url)
    on NotFound   => invoke UseDefault()
    on AuthFailed => invoke UseDefault()  // or: don't handle, let it propagate
};

// ---- P (recovery-defer): cleanup specific to recovery path ----
// Rare — for when restart opens a fallback resource that needs closing
recovery_defer CloseFallbackConn();

// ---- Combined pattern: the full resource acquisition idiom ----
fn Connect(host str) Result<Connection, Error> {
    fd := Open(host)?;
    errdefer Close(fd);

    buf := Alloc(4096)?;
    errdefer Free(buf);

    handshake := DoHandshake(fd, buf)
        restart RetryWithTLS() { return DoTLSHandshake(fd, buf); };
    errdefer |e| Log("handshake failed:", e);

    Validate(handshake)?;

    return ok(Connection{ fd: fd, buf: buf, state: handshake });
    // Success (Z): no errdefers run, caller owns fd, buf, handshake
    // Error (N): errdefers run in reverse — log, free buf, close fd
    // Recovery (P): TLS retry succeeded, errdefers don't run
}
```

---

*The error handling system isn't borrowed from Zig (errdefer), Common Lisp (conditions/restarts), or any other language. It's the N/Z/P trit applied to cleanup semantics — the same three-valued logic that drives typestate, Cottrell operators, and tensor inference. One system at every scale.*
