# Part 4: Conditions and Restarts — The P Path

**Goal:** Add `restart`, `handle`, `invoke`, `on` keywords. The P2 (4) path in the pentit system. When an error occurs, the code at the error site offers recovery options. The caller chooses which one. The stack doesn't unwind — the restart runs at the error site and produces a replacement value. Also add `successdefer` as the P1 (3) path — cleanup that runs on any success (normal or recovered).

**Files modified:** `token.h`, `lexer.c`, `ast.h`, `parser.c`, `cg_stmt.c`, `cg_literal.c`

**Depends on:** Parts 1-3 (DeferEntry with pentit, exit path detection, errdefer/panicdefer)

---

## The mechanism

```
        Caller                           Callee
        ------                           ------

    handle ReadConfig(path) {        fn ReadConfig(path str) Result<Config, Error> {
      on FileNotFound =>                 data := ReadFile(path)
        invoke RetryWith(alt)                restart RetryWith(alt str) {
      on ParseError =>                           return ReadFile(alt);
        invoke UseDefault()                  }
    }                                        restart UseDefault() {
                                                 return "{}";
                                             };

                                             config := Parse(data)?;
                                             return ok(config);
                                         }
```

**Timeline of a recovered call:**
1. Caller sets up handle table: "if FileNotFound -> invoke RetryWith, if ParseError -> invoke UseDefault"
2. Caller calls ReadConfig(path)
3. ReadFile(path) fails with FileNotFound
4. Before unwinding, runtime checks: is there a restart handler for this error?
5. Yes — caller said invoke RetryWith(alt). Runtime calls the restart body with alt.
6. Restart body returns ReadFile(alt) — a replacement value for `data`.
7. Execution continues at the statement AFTER `data := ReadFile(path) restart ...`
8. Exit path = P2 (4). Errdefers don't run. Resources are still live. `recoverdefer` fires.

**Timeline of an unhandled error (no handle block):**
1. ReadFile(path) fails with FileNotFound
2. Before unwinding, runtime checks: is there a restart handler? No.
3. Normal N1 path — errdefers run, defers run, error propagates via `?` or `return err(...)`.

---

## The P-side defer keywords

| Keyword | Trigger | Raw | Fires when |
|---------|---------|-----|------------|
| `successdefer` | P1 | 3 | exit_path >= 3 (normal success or recovery) |
| `recoverdefer` | P2 | 4 | exit_path >= 4 (recovery only) |

**`successdefer`** is the P-side mirror of `errdefer`. It runs on any non-error exit: normal return (Z, which satisfies `2 >= 3`... wait, no. Let's be precise.)

**Correction on the firing rule:** `successdefer` (trigger=3, P1) fires when `exit_path >= 3`. Z (2) does NOT satisfy `2 >= 3`. So `successdefer` fires only on P1 and P2 exits — explicit success paths. This is distinct from `defer` (Z), which fires always.

Use case: `successdefer { commit_transaction(); }` — only commit if we're exiting via success, not on error, and not on normal scope exit that doesn't involve a success signal.

**`recoverdefer`** fires only on P2 (recovery). Use case: `recoverdefer { log("recovered from error") }` — logging that recovery happened.

---

## Step 4.1: New tokens

**File:** `src/token.h`
**Location:** Alphabetically sorted with other keywords

```c
    TOK_KW_ERRDEFER,      /* (added in Part 3) */
    // ...
    TOK_KW_HANDLE,        /* handle expr { ... } */
    // ...
    TOK_KW_INVOKE,        /* invoke RestartName(args) */
    // ...
    TOK_KW_ON,            /* on ErrorType => ... (inside handle block) */
    // ...
    TOK_KW_PANICDEFER,    /* (added in Part 3) */
    // ...
    TOK_KW_RECOVERDEFER,  /* recoverdefer expr; */
    TOK_KW_RESTART,       /* restart Name(params) { body } */
    // ...
    TOK_KW_SUCCESSDEFER,  /* successdefer expr; */
```

Insert each at the correct alphabetical position in the enum. The lexer keyword table must match.

**File:** `src/lexer.c`
**Location:** Keywords table

```c
    {"handle",       TOK_KW_HANDLE},
    // ...
    {"invoke",       TOK_KW_INVOKE},
    // ...
    {"on",           TOK_KW_ON},
    // ...
    {"recoverdefer", TOK_KW_RECOVERDEFER},
    {"restart",      TOK_KW_RESTART},
    // ...
    {"successdefer", TOK_KW_SUCCESSDEFER},
```

And the token-to-string function:
```c
    case TOK_KW_HANDLE:       return "handle";
    case TOK_KW_INVOKE:       return "invoke";
    case TOK_KW_ON:           return "on";
    case TOK_KW_RECOVERDEFER: return "recoverdefer";
    case TOK_KW_RESTART:      return "restart";
    case TOK_KW_SUCCESSDEFER: return "successdefer";
```

---

## Step 4.2: New AST nodes

**File:** `src/ast.h`

### New node kinds:
```c
    ND_RESTART_DECL,     /* restart Name(params) { body } — attached to expression */
    ND_HANDLE_BLOCK,     /* handle expr { on Error => invoke Restart(args), ... } */
    ND_HANDLE_ARM,       /* on ErrorPattern => invoke RestartName(args) */
```

### New union members:
```c
        /* ND_RESTART_DECL: recovery option declared at error site */
        struct {
            const char *name;           /* restart function name */
            uint32_t    name_len;
            AstList    *params;         /* parameter list (ND_PARAM nodes) */
            AstNode    *body;           /* restart body (block or expression) */
        } restart;

        /* ND_HANDLE_BLOCK: caller-side restart dispatch */
        struct {
            AstNode    *expr;           /* the expression to handle (usually a call) */
            AstList    *arms;           /* list of ND_HANDLE_ARM */
        } handle;

        /* ND_HANDLE_ARM: single arm in a handle block */
        struct {
            AstNode    *error_pattern;  /* what error to match (ND_IDENT or ND_PAT_*) */
            const char *restart_name;   /* which restart to invoke */
            uint32_t    restart_name_len;
            AstList    *invoke_args;    /* arguments to pass to the restart */
        } handle_arm;
```

### Extend ND_DEFER for successdefer and recoverdefer:

The `defer` union member from Part 3 already uses `int8_t trigger` — it supports all pentit values. For successdefer (trigger=3) and recoverdefer (trigger=4), the parser sets the trigger accordingly. No new union member needed.

### Extend restartable expressions:

Expressions that can have restart declarations need a way to attach them. Use a wrapper node:

```c
        /* ND_RESTARTABLE: wraps an expression with restart declarations */
        struct {
            AstNode *expr;              /* the expression being protected */
            AstList *restarts;          /* list of ND_RESTART_DECL */
        } restartable;
```

The wrapper approach is cleaner — any expression can be restartable, not just calls. Use a new `ND_RESTARTABLE` node kind.

---

## Step 4.3: Parser — successdefer and recoverdefer

**File:** `src/parser.c`

Extend the defer/errdefer/panicdefer parsing from Part 3 to also handle successdefer and recoverdefer:

```c
    /* Defer / Errdefer / Panicdefer / Successdefer / Recoverdefer */
    if (check(p, TOK_KW_DEFER) || check(p, TOK_KW_ERRDEFER) ||
        check(p, TOK_KW_PANICDEFER) || check(p, TOK_KW_SUCCESSDEFER) ||
        check(p, TOK_KW_RECOVERDEFER)) {

        int8_t trigger = 2;  /* Z: defer (default) */
        bool has_capture = false;

        if (check(p, TOK_KW_PANICDEFER)) {
            trigger = 0;     /* N2: panicdefer */
            has_capture = true;
        } else if (check(p, TOK_KW_ERRDEFER)) {
            trigger = 1;     /* N1: errdefer */
            has_capture = true;
        } else if (check(p, TOK_KW_SUCCESSDEFER)) {
            trigger = 3;     /* P1: successdefer */
        } else if (check(p, TOK_KW_RECOVERDEFER)) {
            trigger = 4;     /* P2: recoverdefer */
        }
        advance(p);

        AstNode *n = make_node(p, ND_DEFER, loc);
        n->u.defer.trigger = trigger;
        n->u.defer.err_capture = NULL;

        /* Error capture only for N-side defers */
        if (has_capture && check(p, TOK_PIPE)) {
            /* ... same |e| capture logic as Part 3 ... */
        }

        /* Body */
        if (check(p, TOK_LBRACE)) {
            n->u.defer.expr = parse_block(p);
        } else {
            n->u.defer.expr = parse_expression(p, 0);
            match(p, TOK_SEMICOLON);
        }
        return n;
    }
```

---

## Step 4.4: Parser — restart declarations

**File:** `src/parser.c`

Restart declarations attach to an expression. They appear after the expression in a statement:

```
data := ReadFile(path)
    restart RetryWith(alt str) { return ReadFile(alt); }
    restart UseDefault() { return "{}"; };
```

### Parse location: after `parse_expression` in variable declaration

In `parse_statement`, after parsing a VarDecl's init expression:

```c
    /* Check for restart declarations after the init expression */
    if (check(p, TOK_KW_RESTART)) {
        AstNode *wrap = make_node(p, ND_RESTARTABLE, init_expr->loc);
        wrap->u.restartable.expr = init_expr;
        wrap->u.restartable.restarts = NULL;

        while (check(p, TOK_KW_RESTART)) {
            advance(p);  /* consume 'restart' */
            SrcLoc rloc = p->cur.loc;

            /* Restart name */
            if (!is_ident(p->cur.kind)) {
                error_at_current(p, "expected restart name");
                break;
            }
            AstNode *rdecl = make_node(p, ND_RESTART_DECL, rloc);
            rdecl->u.restart.name = p->cur.start;
            rdecl->u.restart.name_len = p->cur.len;
            advance(p);  /* consume name */

            /* Parameters */
            expect(p, TOK_LPAREN, "'('");
            rdecl->u.restart.params = NULL;
            if (!check(p, TOK_RPAREN)) {
                rdecl->u.restart.params = parse_param_list(p);
            }
            expect(p, TOK_RPAREN, "')'");

            /* Body */
            rdecl->u.restart.body = parse_block(p);

            wrap->u.restartable.restarts =
                list_append(wrap->u.restartable.restarts, make_list(p, rdecl));
        }

        match(p, TOK_SEMICOLON);  /* trailing semicolon after all restarts */
        init_expr = wrap;  /* replace the init expression with the wrapped version */
    }
```

---

## Step 4.5: Parser — handle blocks

**File:** `src/parser.c`
**Location:** In `parse_expression` or `parse_statement` as a new expression form

```
result := handle ReadConfig("config.json") {
    on FileNotFound => invoke RetryWith("/etc/default.conf")
    on ParseError   => invoke UseDefault()
};
```

```c
    /* Handle block: handle expr { on ... => invoke ..., ... } */
    if (match(p, TOK_KW_HANDLE)) {
        AstNode *n = make_node(p, ND_HANDLE_BLOCK, loc);

        /* The expression to handle (usually a function call) */
        n->u.handle.expr = parse_expression(p, 0);

        expect(p, TOK_LBRACE, "'{'");
        n->u.handle.arms = NULL;

        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            /* on ErrorPattern => invoke RestartName(args) */
            expect(p, TOK_KW_ON, "'on'");
            SrcLoc arm_loc = p->cur.loc;
            AstNode *arm = make_node(p, ND_HANDLE_ARM, arm_loc);

            /* Error pattern: identifier, wildcard, or pattern */
            arm->u.handle_arm.error_pattern = parse_pattern(p);

            expect(p, TOK_FAT_ARROW, "'=>'");

            /* invoke RestartName(args) */
            expect(p, TOK_KW_INVOKE, "'invoke'");
            if (!is_ident(p->cur.kind)) {
                error_at_current(p, "expected restart name after 'invoke'");
            } else {
                arm->u.handle_arm.restart_name = p->cur.start;
                arm->u.handle_arm.restart_name_len = p->cur.len;
                advance(p);
            }

            /* Optional arguments */
            arm->u.handle_arm.invoke_args = NULL;
            if (check(p, TOK_LPAREN)) {
                advance(p);
                arm->u.handle_arm.invoke_args = parse_arg_list(p);
                expect(p, TOK_RPAREN, "')'");
            }

            n->u.handle.arms =
                list_append(n->u.handle.arms, make_list(p, arm));

            /* Optional comma between arms */
            match(p, TOK_COMMA);
        }

        expect(p, TOK_RBRACE, "'}'");
        return n;
    }
```

---

## Step 4.6: Codegen — restartable expressions

**File:** `src/cg_literal.c` or `src/cg_expr.c`

When the codegen encounters `ND_RESTARTABLE`, it:

1. Saves the current insertion point (the "restart point")
2. Emits the inner expression
3. If the inner expression returns a Result, checks is_ok
4. On error: checks the restart choice table
5. For each restart: creates a basic block with the restart body
6. Uses a PHI node to merge the original ok value with restart return values

```c
    case ND_RESTARTABLE: {
        AstNode *inner = node->u.restartable.expr;
        AstList *restarts = node->u.restartable.restarts;

        /* Count restarts */
        int restart_count = 0;
        for (AstList *r = restarts; r; r = r->next) restart_count++;

        if (restart_count == 0) {
            /* No restarts — just emit the inner expression */
            return cg_expr(cg, inner);
        }

        /* Emit the inner expression */
        LLVMValueRef inner_val = cg_expr(cg, inner);

        /* Check if it's a Result type */
        Type *inner_type = type_resolve(cg_expr_type(cg, inner));
        if (!inner_type || inner_type->kind != TY_RESULT) {
            /* Not a Result — restarts don't apply, return value as-is */
            return inner_val;
        }

        /* Extract is_ok */
        LLVMValueRef is_ok = LLVMBuildExtractValue(cg->builder,
            inner_val, 0, "restart.is_ok");

        LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->cur_fn, "restart.ok");
        LLVMBasicBlockRef check_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->cur_fn, "restart.check");
        LLVMBasicBlockRef unwind_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->cur_fn, "restart.unwind");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->cur_fn, "restart.merge");

        LLVMBuildCondBr(cg->builder, is_ok, ok_bb, check_bb);

        /* OK path: extract ok value */
        LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
        Type *ok_type = type_resolve(inner_type->u.result.ok_type);
        LLVMTypeRef ok_lt = cg_llvm_type(cg, ok_type);
        LLVMValueRef ok_data = LLVMBuildExtractValue(cg->builder,
            inner_val, 1, "restart.ok.data");
        LLVMValueRef ok_tmp = cg_entry_alloca(cg, ok_lt, "restart.ok.tmp");
        LLVMBuildStore(cg->builder, ok_data, ok_tmp);
        LLVMValueRef ok_val = LLVMBuildLoad2(cg->builder, ok_lt,
            ok_tmp, "restart.ok.val");
        LLVMBuildBr(cg->builder, merge_bb);

        /* Error path: check restart table */
        LLVMPositionBuilderAtEnd(cg->builder, check_bb);

        /* Extract error value */
        LLVMValueRef err_data = LLVMBuildExtractValue(cg->builder,
            inner_val, 1, "restart.err.data");
        Type *err_type = type_resolve(inner_type->u.result.err_type);
        LLVMTypeRef err_lt = cg_llvm_type(cg, err_type);
        LLVMValueRef err_tmp = cg_entry_alloca(cg, err_lt, "restart.err.tmp");
        LLVMBuildStore(cg->builder, err_data, err_tmp);
        LLVMValueRef err_val = LLVMBuildLoad2(cg->builder, err_lt,
            err_tmp, "restart.err.val");

        /* Check if a handle block registered a restart choice.
         * The restart_choice variable is set by ND_HANDLE_BLOCK before the call.
         * It's an i32: -1 = no handler, 0..N-1 = which restart to invoke.
         * If no handle block -> unwind normally. */
        LLVMValueRef choice = LLVMConstInt(
            LLVMInt32TypeInContext(cg->ctx), (uint64_t)-1, true);
        if (cg->restart_choice_alloca) {
            choice = LLVMBuildLoad2(cg->builder,
                LLVMInt32TypeInContext(cg->ctx),
                cg->restart_choice_alloca, "restart.choice");
        }

        /* Build switch on restart choice */
        LLVMValueRef sw = LLVMBuildSwitch(cg->builder, choice,
            unwind_bb, restart_count);

        /* Emit each restart body as a separate basic block */
        LLVMBasicBlockRef *restart_bbs = arena_alloc(cg->arena,
            sizeof(LLVMBasicBlockRef) * restart_count);
        LLVMValueRef *restart_vals = arena_alloc(cg->arena,
            sizeof(LLVMValueRef) * restart_count);
        int ri = 0;
        for (AstList *r = restarts; r; r = r->next, ri++) {
            AstNode *rdecl = r->node;
            char name_buf[64];
            snprintf(name_buf, sizeof(name_buf), "restart.%.*s",
                     (int)rdecl->u.restart.name_len, rdecl->u.restart.name);

            restart_bbs[ri] = LLVMAppendBasicBlockInContext(
                cg->ctx, cg->cur_fn, name_buf);

            LLVMAddCase(sw,
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), ri, false),
                restart_bbs[ri]);

            /* Emit restart body */
            LLVMPositionBuilderAtEnd(cg->builder, restart_bbs[ri]);
            cg_push_scope(cg);

            /* Bind restart parameters from the handle block's invoke args.
             * The args are stored in restart_args_alloca by ND_HANDLE_BLOCK. */
            /* (Parameter binding details depend on how handle passes args —
             *  see ND_HANDLE_BLOCK codegen below) */

            /* Emit restart body — must return a value of ok_type */
            LLVMValueRef restart_result = cg_expr(cg, rdecl->u.restart.body);

            /* The restart body's return value becomes the replacement */
            restart_vals[ri] = restart_result;

            /* Emit recoverdefer cleanup: exit_path = P2 (4)
             * This fires recoverdefer and successdefer entries. */
            cg_emit_scope_cleanup_pentit(cg, cg->scope, 4);  /* P2: recovery */

            cg_pop_scope(cg);
            if (!cg_block_terminated(cg))
                LLVMBuildBr(cg->builder, merge_bb);
        }

        /* Unwind path: no restart matched -> normal N1 error path */
        LLVMPositionBuilderAtEnd(cg->builder, unwind_bb);
        cg->propagated_err_val = err_val;
        /* The expression's caller (VarDecl or ExprStmt) handles the error.
         * For now, wrap back into Result and return. The ? operator or
         * match handles it from there. */
        LLVMValueRef unwind_val = inner_val;  /* pass through the original error Result */
        LLVMBuildBr(cg->builder, merge_bb);

        /* Merge: PHI node combines ok value, restart values, and unwind value */
        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, ok_lt, "restart.result");

        /* Add ok path */
        LLVMAddIncoming(phi, &ok_val, &ok_bb, 1);

        /* Add each restart path */
        for (int i = 0; i < restart_count; i++) {
            LLVMAddIncoming(phi, &restart_vals[i], &restart_bbs[i], 1);
        }

        /* Add unwind path — this needs special handling since unwind_val
         * is a Result, not an ok_type. For now, use undef as the phi value
         * for the unwind path (it will be overridden by ? or match). */
        LLVMValueRef undef_val = LLVMGetUndef(ok_lt);
        LLVMAddIncoming(phi, &undef_val, &unwind_bb, 1);

        return phi;
    }
```

**NOTE:** This codegen is more complex than defer/errdefer because restarts involve continuation — the function doesn't unwind, it resumes. The PHI node merges the original success value with restart recovery values. The unwind path produces undef because if no restart handles the error, the value is never used (the `?` operator catches it on the next line).

**Recovery exit path:** When a restart body completes, the exit path is P2 (4). This triggers:
- `recoverdefer` (trigger=4, P2): fires because `4 >= 4`
- `successdefer` (trigger=3, P1): fires because `4 >= 3`
- `defer` (trigger=2, Z): fires because Z always fires
- `errdefer` (trigger=1, N1): does NOT fire because `4 <= 1` is false
- `panicdefer` (trigger=0, N2): does NOT fire because `4 <= 0` is false

---

## Step 4.7: Codegen — handle blocks

**File:** `src/cg_expr.c` or `src/cg_literal.c`

The handle block sets up a restart choice BEFORE the call:

```c
    case ND_HANDLE_BLOCK: {
        /* Allocate restart choice variable */
        LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);
        LLVMValueRef choice_alloca = cg_entry_alloca(cg, i32_ty, "restart.choice");
        LLVMBuildStore(cg->builder,
            LLVMConstInt(i32_ty, (uint64_t)-1, true), choice_alloca);

        /* Store the choice alloca in the codegen context so the
         * ND_RESTARTABLE codegen can find it */
        LLVMValueRef saved_choice = cg->restart_choice_alloca;
        cg->restart_choice_alloca = choice_alloca;

        /* TODO: Allocate space for restart arguments
         * The handle arms specify invoke args that need to be available
         * when the restart body runs. Store them in allocas. */

        /* Emit the inner expression (which contains restart declarations) */
        LLVMValueRef result = cg_expr(cg, node->u.handle.expr);

        /* Restore saved choice alloca */
        cg->restart_choice_alloca = saved_choice;

        /* The handle arms need to be evaluated BEFORE the call but
         * applied DURING the call. This requires the restart choice
         * mechanism — the handle block stores which restart to invoke
         * and the restartable expression checks it.
         *
         * Full implementation: the handle block emits a "chooser" function
         * that takes an error value and returns (restart_index, args).
         * The restartable expression calls this chooser when an error occurs.
         */

        return result;
    }
```

**DESIGN NOTE:** The handle/restart interaction is the most complex part of this system. The full implementation requires:
1. Handle block emits a "chooser" that maps error patterns to restart indices
2. Restart declarations emit basic blocks at the call site
3. The chooser and the restart blocks communicate via allocas
4. The PHI node at the merge point produces the final value

This is achievable with LLVM's existing infrastructure (switch, phi, basic blocks). No runtime support needed — it's all compile-time code generation.

---

## Step 4.8: Add restart_choice_alloca to Codegen struct

**File:** `src/codegen.h`
**Location:** Inside Codegen struct

```c
    /* Composable error system: restart choice mechanism */
    LLVMValueRef     restart_choice_alloca;  /* set by ND_HANDLE_BLOCK, read by ND_RESTARTABLE */
```

---

## Step 4.9: Update ND_DEFER codegen for successdefer and recoverdefer

**File:** `src/cg_stmt.c`
**Location:** `case ND_DEFER` (updated in Part 3)

Add cases for the P-side triggers:

```c
    case ND_DEFER:
        if (node->u.defer.expr) {
            int8_t trigger = node->u.defer.trigger;
            switch (trigger) {
            case 0:  /* N2: panicdefer */
                cg_push_panicdefer(cg, node->u.defer.expr);
                if (node->u.defer.err_capture) {
                    cg->scope->defers[cg->scope->defer_count - 1].err_capture =
                        node->u.defer.err_capture;
                }
                break;
            case 1:  /* N1: errdefer */
                cg_push_errdefer(cg, node->u.defer.expr, node->u.defer.err_capture);
                break;
            case 2:  /* Z: defer */
                cg_push_defer(cg, node->u.defer.expr);
                break;
            case 3:  /* P1: successdefer */
                cg_push_successdefer(cg, node->u.defer.expr);
                break;
            case 4:  /* P2: recoverdefer */
                cg_push_recoverdefer(cg, node->u.defer.expr);
                break;
            }
        }
        break;
```

---

## Step 4.10: Tests

### Test 1: Basic restart with handle
```
fn ReadData(path str) Result<str, i32> {
    return err(-1);  // simulate failure
    restart UseDefault() { return ok("default data"); }
}

fn Main() {
    data := handle ReadData("missing.txt") {
        on _ => invoke UseDefault()
    };
    Println(data);  // prints "default data"
}
```

### Test 2: Restart preserves resources (no errdefer fired)
```
extern fn printf(fmt *u8, ...) i32;

fn Process() Result<i32, i32> {
    buf := 42;
    errdefer printf(CStr("THIS SHOULD NOT PRINT\n"));

    val := Compute()
        restart UseZero() { return ok(0); };

    return ok(buf + val);
}

fn Compute() Result<i32, i32> { return err(-1); }

fn Main() {
    r := handle Process() {
        on _ => invoke UseZero()
    };
    match r {
        ok(v) => { Println(v); }    // prints 42 (buf + 0)
        err(e) => { Println(e); }
    }
    // errdefer did NOT print — P2 path, not N1 path
}
```

### Test 3: No handle block -> normal error propagation
```
extern fn printf(fmt *u8, ...) i32;

fn Risky() Result<i32, i32> {
    errdefer printf(CStr("errdefer fired\n"));

    val := Compute()
        restart UseZero() { return ok(0); };

    return ok(val);
}

fn Compute() Result<i32, i32> { return err(-1); }

fn Main() {
    r := Risky();  // no handle block — restart not invoked
    // errdefer SHOULD print — N1 path
    match r {
        ok(v) => { Println(v); }
        err(e) => { Println(e); }    // prints -1
    }
}
```

### Test 4: recoverdefer fires on recovery
```
extern fn printf(fmt *u8, ...) i32;

fn WithRecoverDefer() Result<i32, i32> {
    recoverdefer printf(CStr("recoverdefer: recovery happened\n"));
    errdefer printf(CStr("errdefer: THIS SHOULD NOT PRINT\n"));

    val := Compute()
        restart UseZero() { return ok(0); };

    return ok(val);
}

fn Compute() Result<i32, i32> { return err(-1); }

fn Main() {
    r := handle WithRecoverDefer() {
        on _ => invoke UseZero()
    };
    // Output: "recoverdefer: recovery happened"
    // errdefer did NOT print
    match r {
        ok(v) => { Println(v); }    // prints 0
        err(e) => { Println(e); }
    }
}
```

### Test 5: successdefer fires on P1 and P2, not on N1
```
extern fn printf(fmt *u8, ...) i32;

fn WithSuccessDefer() Result<i32, i32> {
    successdefer printf(CStr("successdefer: success path\n"));
    errdefer printf(CStr("errdefer: error path\n"));

    val := Compute()
        restart UseZero() { return ok(0); };

    return ok(val);
}

fn Compute() Result<i32, i32> { return err(-1); }

fn Main() {
    // With handle -> recovery -> P2 path
    r := handle WithSuccessDefer() {
        on _ => invoke UseZero()
    };
    // Output: "successdefer: success path" (P2 >= P1, so successdefer fires)
    // errdefer did NOT print
    match r {
        ok(v) => { Println(v); }
        err(e) => { Println(e); }
    }
}
```

---

## What this step accomplishes

1. **`restart` keyword and declarations work** — attached to expressions
2. **`handle` blocks work** — caller specifies recovery policy
3. **`invoke` dispatches to named restarts** — error matched, restart chosen
4. **`successdefer` keyword works** — fires on P1 (success) and P2 (recovery) exits
5. **`recoverdefer` keyword works** — fires only on P2 (recovery) exits
6. **P2 path is functional** — errdefers don't fire on recovery
7. **No handle = no recovery** — falls through to N1 path normally
8. **Resources preserved on P path** — ownership state maintained

The pentit system is complete: N2 (panicdefer), N1 (errdefer), Z (defer), P1 (successdefer), P2 (recoverdefer + restarts). All five pentit values active.

**The composability principle in action:** Most code uses only `defer` + `errdefer` (bool level). Add `recoverdefer` for trit level. Add `panicdefer` + `successdefer` for pentit level. The developer picks the resolution that fits.
