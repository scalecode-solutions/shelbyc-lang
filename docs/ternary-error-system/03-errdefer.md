# Part 3: Errdefer — The N1 Path

**Goal:** Add `errdefer` keyword with optional error capture `|e|`. Parser, lexer, codegen. The N1 (1) trigger in the pentit defer system. Also note that `panicdefer` uses the same mechanism at N2 (0) and is added alongside errdefer.

**Files modified:** `token.h`, `lexer.c`, `ast.h`, `parser.c`, `cg_stmt.c`

**Depends on:** Part 1 (DeferEntry struct), Part 2 (exit path pentit assignments)

---

## Step 3.1: Add TOK_KW_ERRDEFER and TOK_KW_PANICDEFER tokens

**File:** `src/token.h`
**Location:** After `TOK_KW_DEFER` (line 48)

### Before:
```c
    TOK_KW_DEFER,
    TOK_KW_ELSE,
```

### After:
```c
    TOK_KW_DEFER,
    TOK_KW_ERRDEFER,
    TOK_KW_ELSE,
    // ...
    TOK_KW_PANICDEFER,
```

**WARNING:** Adding tokens shifts all subsequent enum values. Every token kind after this point increments by 1. Since the C bootstrap uses the enum symbolically (not hardcoded integers), this is safe. But the ShelbyC self-hosted compiler uses hardcoded tag values — the shelbyc rewrite must account for this.

**Note on `panicdefer`:** It shares the same AST representation and codegen path as `errdefer` — the only difference is the trigger value. `errdefer` pushes trigger=1 (N1), `panicdefer` pushes trigger=0 (N2). Both support `|e|` error capture. The parser handles both with the same code path, differing only in the trigger assigned.

---

## Step 3.2: Add keyword recognition in lexer

**File:** `src/lexer.c`
**Location:** Keywords table (line 21+), alphabetically sorted

### Before:
```c
    {"defer",     TOK_KW_DEFER},
    {"else",      TOK_KW_ELSE},
```

### After:
```c
    {"defer",      TOK_KW_DEFER},
    {"errdefer",   TOK_KW_ERRDEFER},
    {"else",       TOK_KW_ELSE},
    // ...
    {"panicdefer", TOK_KW_PANICDEFER},
```

**Also add to the token-to-string function** (line ~119):
```c
    case TOK_KW_ERRDEFER:     return "errdefer";
    case TOK_KW_PANICDEFER:   return "panicdefer";
```

---

## Step 3.3: Extend ND_DEFER AST node

**File:** `src/ast.h`
**Location:** The `single` union member used by ND_DEFER (line ~264)

### Before:
```c
        /* ND_RETURN, ND_BREAK, ND_DEFER, ND_WAKE, ND_EXPR_STMT, ND_SOME, ND_OK, ND_ERR */
        struct { AstNode *expr; const char *label; uint32_t label_len; } single;
```

### After:
Two options. Option A is to add a new union member for defer:

```c
        /* ND_RETURN, ND_BREAK, ND_WAKE, ND_EXPR_STMT, ND_SOME, ND_OK, ND_ERR */
        struct { AstNode *expr; const char *label; uint32_t label_len; } single;

        /* ND_DEFER: unified defer/errdefer/panicdefer (and later successdefer/recoverdefer) */
        struct {
            AstNode *expr;          /* the deferred expression or block */
            int8_t   trigger;       /* pentit raw: 0=N2(panicdefer), 1=N1(errdefer), 2=Z(defer) */
            AstNode *err_capture;   /* errdefer |e| { ... } — ident node, NULL if none */
        } defer;
```

Option B is to keep ND_DEFER using `single` and add a flag elsewhere. Option A is cleaner because the defer-specific fields (trigger, err_capture) don't apply to ND_RETURN or ND_BREAK.

**Go with Option A.** ND_DEFER gets its own union member `defer`.

**Update the comment on ND_DEFER:**
```c
    ND_DEFER,           /* defer expr; or errdefer [|e|] expr; or panicdefer [|e|] expr; */
```

---

## Step 3.4: Parse errdefer and panicdefer

**File:** `src/parser.c`
**Location:** After the existing defer parsing (line 1966-1972)

### Before:
```c
    /* Defer */
    if (match(p, TOK_KW_DEFER)) {
        AstNode *n = make_node(p, ND_DEFER, loc);
        n->u.single.expr = parse_expression(p, 0);
        match(p, TOK_SEMICOLON);
        return n;
    }
```

### After:
```c
    /* Defer / Errdefer / Panicdefer */
    if (check(p, TOK_KW_DEFER) || check(p, TOK_KW_ERRDEFER) || check(p, TOK_KW_PANICDEFER)) {
        int8_t trigger = 2;  /* Z: defer (default) */
        bool has_capture = false;
        if (check(p, TOK_KW_ERRDEFER)) {
            trigger = 1;     /* N1: errdefer */
            has_capture = true;
        } else if (check(p, TOK_KW_PANICDEFER)) {
            trigger = 0;     /* N2: panicdefer */
            has_capture = true;
        }
        advance(p);  /* consume defer, errdefer, or panicdefer */

        AstNode *n = make_node(p, ND_DEFER, loc);
        n->u.defer.trigger = trigger;
        n->u.defer.err_capture = NULL;

        /* errdefer |e| { ... } or panicdefer |e| { ... } — optional error capture */
        if (has_capture && check(p, TOK_PIPE)) {
            advance(p);  /* consume | */
            SrcLoc cap_loc = p->cur.loc;
            if (!is_ident(p->cur.kind)) {
                error_at_current(p, "expected identifier after '|' in errdefer/panicdefer capture");
            } else {
                AstNode *ident = make_node(p, ND_IDENT, cap_loc);
                ident->u.ident.name = p->cur.start;
                ident->u.ident.name_len = p->cur.len;
                advance(p);  /* consume identifier */
                n->u.defer.err_capture = ident;
            }
            expect(p, TOK_PIPE, "'|'");  /* closing | */
        }

        /* Body: either a block { ... } or a single expression */
        if (check(p, TOK_LBRACE)) {
            n->u.defer.expr = parse_block(p);
        } else {
            n->u.defer.expr = parse_expression(p, 0);
            match(p, TOK_SEMICOLON);
        }

        return n;
    }
```

**What this parses:**

| Syntax | trigger | err_capture | expr |
|--------|---------|-------------|------|
| `defer Close(fd);` | 2 (Z) | NULL | Call(Close, fd) |
| `defer { Cleanup(); Log("done"); }` | 2 (Z) | NULL | Block(...) |
| `errdefer Free(buf);` | 1 (N1) | NULL | Call(Free, buf) |
| `errdefer \|e\| Log("failed:", e);` | 1 (N1) | Ident("e") | Call(Log, ...) |
| `errdefer \|e\| { Log(e); Rollback(); }` | 1 (N1) | Ident("e") | Block(...) |
| `panicdefer CoreDump();` | 0 (N2) | NULL | Call(CoreDump) |
| `panicdefer \|e\| { Log(e); Abort(); }` | 0 (N2) | Ident("e") | Block(...) |

---

## Step 3.5: Update codegen for ND_DEFER

**File:** `src/cg_stmt.c`
**Location:** `case ND_DEFER` (line 1469-1473)

### Before:
```c
    case ND_DEFER:
        /* Push deferred expression onto the stack (emitted at return) */
        if (node->u.single.expr)
            cg_push_defer(cg, node->u.single.expr);
        break;
```

### After:
```c
    case ND_DEFER:
        if (node->u.defer.expr) {
            int8_t trigger = node->u.defer.trigger;
            switch (trigger) {
            case 0:  /* N2: panicdefer */
                cg_push_panicdefer(cg, node->u.defer.expr);
                /* panicdefer with |e| capture: store on the entry */
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
            default:
                /* P1 (successdefer) and P2 (recoverdefer) added in later parts */
                cg_push_defer(cg, node->u.defer.expr);
                break;
            }
        }
        break;
```

**What changed:**
- Accesses `node->u.defer.expr` instead of `node->u.single.expr`
- Reads the pentit `trigger` value directly from the AST node
- Dispatches to the appropriate push function: `cg_push_panicdefer` (N2=0), `cg_push_errdefer` (N1=1), or `cg_push_defer` (Z=2)
- Passes `err_capture` for errdefer/panicdefer (the binding node for `|e|`)

The actual emission of errdefers/panicdefers is handled by `cg_emit_scope_defers_pentit` (Part 1) when an N-side exit path is detected (Part 2). No additional codegen changes needed here.

**Firing behavior recap from the pentit truth table:**
- `panicdefer` (trigger=0, N2): fires when exit_path <= 0 (only on panic)
- `errdefer` (trigger=1, N1): fires when exit_path <= 1 (on panic OR error)
- `defer` (trigger=2, Z): fires always

This means errdefer catches panics too — a panic is "at least as extreme" as an error. If you want cleanup ONLY on panic (not on normal errors), use panicdefer. Most developers only need defer + errdefer (bool level).

---

## Step 3.6: Update the old defer parsing to use new union member

The existing `defer` keyword parsing (now merged with errdefer/panicdefer in Step 3.4) switches from `n->u.single.expr` to `n->u.defer.expr`. But other nodes that shared the `single` union (ND_RETURN, ND_BREAK, etc.) are NOT affected — they continue using `u.single`.

Any code that previously accessed `node->u.single.expr` for ND_DEFER nodes must be updated to `node->u.defer.expr`. Search for all such access points:

```bash
grep -rn "u\.single.*ND_DEFER\|ND_DEFER.*u\.single" src/*.c
```

In the C bootstrap, the only access is in `cg_stmt.c:1471` (already updated in Step 3.5). If there are any in flow analysis or other passes, update them too.

---

## Step 3.7: Tests

### Test 1: Basic errdefer
```
fn Setup() Result<i32, i32> {
    x := 0;
    errdefer x = 99;   // only runs on error path

    if false {
        return err(-1);  // N1 path: errdefer runs, x becomes 99
    }
    return ok(42);       // Z path: errdefer does NOT run
}

fn Main() {
    r := Setup();
    // r should be ok(42) — errdefer didn't run
    match r {
        ok(v)  => { Println(v); }    // prints 42
        err(e) => { Println(e); }
    }
}
```

### Test 2: Errdefer with ? propagation
```
fn Fallible(n i32) Result<i32, i32> {
    if n < 0 { return err(n); }
    return ok(n * 2);
}

extern fn printf(fmt *u8, ...) i32;

fn Chain(a i32, b i32) Result<i32, i32> {
    x := Fallible(a)?;
    errdefer printf(CStr("errdefer: x was %d\n"), x);

    y := Fallible(b)?;   // if b < 0, errdefer prints x
    return ok(x + y);
}

fn Main() {
    r := Chain(5, -1);
    // errdefer should have printed "errdefer: x was 10"
    match r {
        ok(v)  => { Println(v); }
        err(e) => { Println(e); }    // prints -1
    }
}
```

### Test 3: Errdefer with |e| capture
```
extern fn printf(fmt *u8, ...) i32;

fn Risky() Result<i32, i32> {
    errdefer |e| {
        printf(CStr("caught error: %d\n"), e);
    }
    return err(42);
}

fn Main() {
    r := Risky();
    // should print "caught error: 42"
    match r {
        ok(v)  => { Println(v); }
        err(e) => { Println(e); }    // prints 42
    }
}
```

### Test 4: Mixed defer + errdefer ordering (LIFO)
```
extern fn printf(fmt *u8, ...) i32;

fn Ordered() Result<i32, i32> {
    defer printf(CStr("defer 1\n"));
    errdefer printf(CStr("errdefer 2\n"));
    defer printf(CStr("defer 3\n"));
    errdefer printf(CStr("errdefer 4\n"));
    return err(-1);
}

fn Main() {
    r := Ordered();
    // Error path (N1): all 4 run in REVERSE order
    // errdefer fires because exit_path=1 <= trigger=1
    // defer fires because Z always fires
    // Output should be:
    //   errdefer 4
    //   defer 3
    //   errdefer 2
    //   defer 1
}
```

### Test 5: Errdefer does NOT run on success
```
extern fn printf(fmt *u8, ...) i32;

fn Success() Result<i32, i32> {
    errdefer printf(CStr("THIS SHOULD NOT PRINT\n"));
    return ok(42);
}

fn Main() {
    r := Success();
    match r {
        ok(v) => { Println(v); }     // prints 42
        err(e) => { Println(e); }
    }
    // No errdefer output — Z path, exit_path=2 is NOT <= trigger=1
}
```

### Test 6: Nested scopes with errdefer
```
extern fn printf(fmt *u8, ...) i32;

fn Fallible(n i32) Result<i32, i32> {
    if n < 0 { return err(n); }
    return ok(n);
}

fn Nested() Result<i32, i32> {
    errdefer printf(CStr("outer errdefer\n"));

    x := Fallible(1)?;
    errdefer printf(CStr("inner errdefer\n"));

    y := Fallible(-1)?;  // fails here
    return ok(x + y);
}

fn Main() {
    r := Nested();
    // Output:
    //   inner errdefer
    //   outer errdefer
    match r {
        ok(v) => { Println(v); }
        err(e) => { Println(e); }    // prints -1
    }
}
```

### Test 7: Panicdefer fires only on panic, errdefer fires on error AND panic
```
extern fn printf(fmt *u8, ...) i32;

fn WithPanicDefer() Result<i32, i32> {
    panicdefer printf(CStr("panicdefer: panic cleanup\n"));
    errdefer printf(CStr("errdefer: error cleanup\n"));

    return err(-1);  // N1 path
    // errdefer fires (exit_path=1 <= trigger=1)
    // panicdefer does NOT fire (exit_path=1 is NOT <= trigger=0)
}

fn Main() {
    r := WithPanicDefer();
    // Output: only "errdefer: error cleanup"
    match r {
        ok(v) => { Println(v); }
        err(e) => { Println(e); }
    }
}
```

---

## Step 3.8: Verify

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
# Run all existing tests first — no regression
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Then run the 7 new errdefer/panicdefer tests
```

**Expected:** 772/772 existing + 7/7 new = all pass.

---

## What this step accomplishes

1. **`errdefer` keyword exists** — lexed, parsed, codegen'd with trigger=1 (N1)
2. **`panicdefer` keyword exists** — lexed, parsed, codegen'd with trigger=0 (N2)
3. **Error capture `|e|` works** — the error value is bound and accessible in the errdefer/panicdefer body
4. **Block bodies work** — `errdefer |e| { ... }` with multiple statements
5. **LIFO ordering preserved** — defer, errdefer, and panicdefer interleave in declaration order, emit in reverse
6. **N1 path is fully functional** — errdefers run on `?` error, `return err(...)`, runtime Result check
7. **N2 path is wired** — panicdefer runs only on panic (more extreme than N1)
8. **Z path unaffected** — errdefers and panicdefers don't run on `return ok(...)` or normal scope exit

The N-side pentit paths are live. The Z path is unchanged. The P-side slots (P1=successdefer, P2=recoverdefer) exist in the DeferEntry but aren't wired yet — that's Part 4.
