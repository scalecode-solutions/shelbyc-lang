# Part 3: Errdefer — The N Path

**Goal:** Add `errdefer` keyword with optional error capture `|e|`. Parser, lexer, codegen. The N (-1) trigger in the ternary defer system.

**Files modified:** `token.h`, `lexer.c`, `ast.h`, `parser.c`, `cg_stmt.c`

**Depends on:** Part 1 (DeferEntry struct), Part 2 (exit path trit assignments)

---

## Step 3.1: Add TOK_KW_ERRDEFER token

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
```

**WARNING:** Adding a token shifts all subsequent enum values. Every token kind after this point increments by 1. Since the C bootstrap uses the enum symbolically (not hardcoded integers), this is safe. But the ShelbyC self-hosted compiler uses hardcoded tag values — the shelbyc rewrite must account for this.

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
    {"defer",     TOK_KW_DEFER},
    {"errdefer",  TOK_KW_ERRDEFER},
    {"else",      TOK_KW_ELSE},
```

**Also add to the token-to-string function** (line ~119):
```c
    case TOK_KW_ERRDEFER:     return "errdefer";
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

        /* ND_DEFER: unified defer/errdefer/recovery-defer */
        struct {
            AstNode *expr;          /* the deferred expression or block */
            bool     is_errdefer;   /* false=defer(Z), true=errdefer(N) */
            AstNode *err_capture;   /* errdefer |e| { ... } — ident node, NULL if none */
        } defer;
```

Option B is to keep ND_DEFER using `single` and add a flag elsewhere. Option A is cleaner because the defer-specific fields (is_errdefer, err_capture) don't apply to ND_RETURN or ND_BREAK.

**Go with Option A.** ND_DEFER gets its own union member `defer`.

**Update the comment on ND_DEFER:**
```c
    ND_DEFER,           /* defer expr; or errdefer [|e|] expr; */
```

---

## Step 3.4: Parse errdefer

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
    /* Defer / Errdefer */
    if (check(p, TOK_KW_DEFER) || check(p, TOK_KW_ERRDEFER)) {
        bool is_err = check(p, TOK_KW_ERRDEFER);
        advance(p);  /* consume defer or errdefer */

        AstNode *n = make_node(p, ND_DEFER, loc);
        n->u.defer.is_errdefer = is_err;
        n->u.defer.err_capture = NULL;

        /* errdefer |e| { ... } — optional error capture */
        if (is_err && check(p, TOK_PIPE)) {
            advance(p);  /* consume | */
            SrcLoc cap_loc = p->cur.loc;
            if (!is_ident(p->cur.kind)) {
                error_at_current(p, "expected identifier after '|' in errdefer capture");
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

| Syntax | is_errdefer | err_capture | expr |
|--------|------------|-------------|------|
| `defer Close(fd);` | false | NULL | Call(Close, fd) |
| `defer { Cleanup(); Log("done"); }` | false | NULL | Block(...) |
| `errdefer Free(buf);` | true | NULL | Call(Free, buf) |
| `errdefer \|e\| Log("failed:", e);` | true | Ident("e") | Call(Log, ...) |
| `errdefer \|e\| { Log(e); Rollback(); }` | true | Ident("e") | Block(...) |

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
            if (node->u.defer.is_errdefer)
                cg_push_errdefer(cg, node->u.defer.expr, node->u.defer.err_capture);
            else
                cg_push_defer(cg, node->u.defer.expr);
        }
        break;
```

**What changed:**
- Accesses `node->u.defer.expr` instead of `node->u.single.expr`
- Checks `is_errdefer` flag
- Calls `cg_push_errdefer` (trigger=-1) for errdefer, `cg_push_defer` (trigger=0) for defer
- Passes `err_capture` to errdefer push (the binding node for `|e|`)

The actual emission of errdefers is handled by `cg_emit_scope_defers_trit` (Part 1) when an N exit path is detected (Part 2). No additional codegen changes needed here.

---

## Step 3.6: Update the old defer parsing to use new union member

The existing `defer` keyword parsing (now merged with errdefer in Step 3.4) switches from `n->u.single.expr` to `n->u.defer.expr`. But other nodes that shared the `single` union (ND_RETURN, ND_BREAK, etc.) are NOT affected — they continue using `u.single`.

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
        return err(-1);  // N path: errdefer runs, x becomes 99
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
    // Error path: all 4 run in REVERSE order
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
    // No errdefer output — success path
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

---

## Step 3.8: Verify

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
# Run all existing tests first — no regression
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Then run the 6 new errdefer tests
```

**Expected:** 768/768 existing + 6/6 new = all pass.

---

## What this step accomplishes

1. **`errdefer` keyword exists** — lexed, parsed, codegen'd
2. **Error capture `|e|` works** — the error value is bound and accessible in the errdefer body
3. **Block bodies work** — `errdefer |e| { ... }` with multiple statements
4. **LIFO ordering preserved** — defer and errdefer interleave in declaration order, emit in reverse
5. **N path is fully functional** — errdefers run on `?` error, `return err(...)`, runtime Result check
6. **Z path unaffected** — errdefers don't run on `return ok(...)` or normal scope exit

The N trit is live. The Z trit is unchanged. The P trit slot exists but isn't wired yet — that's Part 4.
