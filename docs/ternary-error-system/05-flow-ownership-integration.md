# Part 5: Flow Ownership Integration

**Goal:** Connect the ternary defer system to Flow Ownership. The ownership solver must understand all three exit paths and verify safety on each.

**Files modified:** `flow.h`, `flow.c`

**Depends on:** Parts 1-4 (all three paths functional)

---

## Current ownership state enum

**File:** `src/flow.h` (line 131-136)

```c
typedef enum {
    OS_ALIVE,           /* value exists and is usable */
    OS_CONSUMED,        /* value has been moved away */
    OS_MAYBE_CONSUMED,  /* consumed on some paths, alive on others (merge point) */
    OS_DROPPED,         /* value has been explicitly dropped */
} OwnershipState;
```

---

## Mapping to trits

| OwnershipState | Trit | Meaning |
|---------------|------|---------|
| `OS_ALIVE` | P (+1) | owned, live, usable |
| `OS_CONSUMED` | N (-1) | moved away, gone |
| `OS_DROPPED` | N (-1) | explicitly destroyed |
| `OS_MAYBE_CONSUMED` | merge conflict | N on some paths, P on others |

**New addition needed:**
```c
typedef enum {
    OS_ALIVE,           /* P(+1): value exists and is usable */
    OS_BORROWED,        /* Z(0):  temporarily lent, will return */
    OS_CONSUMED,        /* N(-1): value has been moved away */
    OS_MAYBE_CONSUMED,  /* merge: consumed on some paths, alive on others */
    OS_DROPPED,         /* N(-1): explicitly dropped */
} OwnershipState;
```

`OS_BORROWED` = Z(0). The variable is temporarily lent to a borrow. It's not fully alive (can't be moved) and not consumed (will come back). This is the zero trit — the neutral state between alive and consumed.

---

## Merge function update

**File:** `src/flow.c` (line 978-981)

### Before:
```c
static OwnershipState ownership_merge(OwnershipState a, OwnershipState b) {
    if (a == b) return a;
    return OS_MAYBE_CONSUMED;
}
```

### After:
```c
/*
 * Merge ownership states at a control flow join point.
 *
 * This is a trit operation: the consensus of two trit values.
 *
 *   P(alive) merge P(alive) = P(alive)         — both alive
 *   N(consumed) merge N(consumed) = N(consumed) — both consumed
 *   Z(borrowed) merge Z(borrowed) = Z(borrowed) — both borrowed
 *
 *   P(alive) merge N(consumed) = MAYBE_CONSUMED — conflict
 *   P(alive) merge Z(borrowed) = Z(borrowed)    — conservatively borrowed
 *   Z(borrowed) merge N(consumed) = MAYBE_CONSUMED — conflict
 *
 * The Cottrell Confluence ** (consensus) operator:
 *   P ** P = P, N ** N = N, Z ** Z = Z
 *   P ** N = Z (disagree → neutral)
 *   P ** Z = Z, N ** Z = Z (one neutral → neutral)
 *
 * For ownership merge, the "neutral" result is either BORROWED (if
 * neither is consumed) or MAYBE_CONSUMED (if either is consumed).
 */
static OwnershipState ownership_merge(OwnershipState a, OwnershipState b) {
    if (a == b) return a;

    /* If either is consumed/dropped and the other isn't → conflict */
    bool a_gone = (a == OS_CONSUMED || a == OS_DROPPED);
    bool b_gone = (b == OS_CONSUMED || b == OS_DROPPED);
    if (a_gone || b_gone) return OS_MAYBE_CONSUMED;

    /* Both alive or borrowed, but different → conservative = borrowed */
    if ((a == OS_ALIVE && b == OS_BORROWED) ||
        (a == OS_BORROWED && b == OS_ALIVE))
        return OS_BORROWED;

    return OS_MAYBE_CONSUMED;
}
```

---

## How the three exit paths affect ownership

### Z path (normal return): standard ownership transfer
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    conn := Connect(host)?;  // conn: OS_ALIVE

    result := Process(buf, conn);

    return ok(result);
    // buf: ownership transferred to Process → OS_CONSUMED
    // conn: ownership transferred to Process → OS_CONSUMED
    // result: returned to caller → ownership transferred out
    // Z path: defers run, drops run for any remaining alive vars
}
```

### N path (error return): errdefer cleanup
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // registered on N path

    conn := Connect(host)?;  // If this fails: N path
    errdefer Close(conn);     // registered on N path

    // When Connect fails at ?:
    // exit_path = N (-1)
    // errdefer Close(conn) runs → conn: OS_CONSUMED (by errdefer)
    // errdefer Free(buf) runs → buf: OS_CONSUMED (by errdefer)
    // All resources cleaned up. No leaks.
}
```

**Flow analysis verification:**
- After `?` error branch: conn is NOT OS_ALIVE (it was never assigned on error)
- After errdefer Free(buf): buf transitions to OS_CONSUMED
- At function exit: all variables are OS_CONSUMED or OS_DROPPED
- **SAFE**: no use-after-free, no leak

### P path (restart recovery): ownership preserved
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // registered on N path

    data := ReadFile(path)    // If this fails:
        restart UseCache() {  //   restart provides replacement
            return cache.Get(path);
        };
    // After restart: data is OS_ALIVE (restart produced a value)
    // buf: still OS_ALIVE (errdefer did NOT run — P path)

    Process(buf, data);
    return ok(result);
}
```

**Flow analysis verification:**
- After restart: `data` is OS_ALIVE (restart's return value)
- `buf` is still OS_ALIVE (errdefer didn't fire on P path)
- **SAFE**: both variables are live and owned, same as the Z path

---

## The ownership effect of each defer trigger

**During flow analysis, each DeferEntry generates ownership effects:**

| Trigger | When it fires | Effect on referenced variables |
|---------|--------------|-------------------------------|
| Z (defer) | N and Z paths | Variables used in defer body are READ on those paths |
| N (errdefer) | N path only | Variables used in errdefer body are READ/CONSUMED on N path |
| P (recovery) | P path only | Variables used in recovery body are READ/CONSUMED on P path |

**Example analysis:**
```
buf := Alloc(1024)?;        // Effect: WRITE(buf) → buf becomes OS_ALIVE
errdefer Free(buf);          // Effect on N path: CONSUME(buf) → buf becomes OS_CONSUMED
defer Log("done");           // Effect on N+Z paths: READ(log_str) → no ownership change
data := ReadFile(path)?;     // Effect: WRITE(data) → data becomes OS_ALIVE
```

The flow analysis tracks these effects per-path:

**Z path (success):**
- buf: ALIVE → (passed to Process) → CONSUMED ✓
- data: ALIVE → (passed to Process) → CONSUMED ✓
- defer Log: READ ✓

**N path (error at ReadFile):**
- buf: ALIVE → errdefer Free → CONSUMED ✓
- data: never assigned (error happened before) → CONSUMED (trivially) ✓
- defer Log: READ ✓
- errdefer Free: CONSUME(buf) ✓

**P path (restart at ReadFile):**
- buf: ALIVE → still ALIVE (errdefer didn't fire) ✓
- data: ALIVE (restart produced replacement) → (used later) ✓
- errdefer Free: NOT fired → buf not consumed ✓

---

## Coupled solver integration

From the coupled-ownership-solver blog post:

Each solver level receives failures as structured constraints. A restart point introduces a new kind of constraint:

```c
typedef struct {
    int         var_id;         /* which variable */
    ConstraintKind kind;        /* what kind of constraint */
    SrcLoc      location;
    OwnershipState state;       /* trit-mapped ownership */
    int8_t      exit_path;      /* NEW: which trit path this constraint applies to */
} OwnershipConstraint;
```

**L1 analysis with ternary paths:**

L1 propagates ownership forward through the CFG. At a restart point:
- **Z path edge:** data = OS_ALIVE (ReadFile succeeded)
- **N path edge:** data = not assigned, buf = OS_CONSUMED (errdefer)
- **P path edge:** data = OS_ALIVE (restart succeeded), buf = OS_ALIVE (errdefer skipped)

L1 merges these at the join point after the restart:
- `data`: OS_ALIVE on Z and P, not relevant on N (function returns) → OS_ALIVE ✓
- `buf`: OS_ALIVE on Z and P, OS_CONSUMED on N → at join: OS_ALIVE (N exits function)

L1 succeeds — no escalation to L2 needed. The restart point didn't introduce a conflict because the P path preserves the same ownership state as Z.

**When L1 DOES escalate (complex case):**

If the restart body borrows from a variable that's also used later, L1 might detect a conflict:
```
restart RetryWith(alt) {
    return Transform(buf, alt);  // borrows buf
}
// ... later ...
Free(buf);  // consumes buf — conflict if restart ran?
```

L1 reports: `{borrow: buf@restart_body, conflict: consume_buf@later}`. L2 receives this and creates a region for the restart scope. L2 verifies the borrow doesn't outlive the restart body. Since the restart body returns before `Free(buf)` runs, L2 proves it safe.

The coupled solver handles restart points the same way it handles any other scope — the failure propagation mechanism doesn't need changes. The new information is just the `exit_path` field on constraints, which tells L2 which path the conflict is on.

---

## Verification strategy

For each test program:
1. Run flow analysis with the ternary path information
2. Verify that on the N path, errdefer-consumed variables are OS_CONSUMED
3. Verify that on the P path, variables are the same state as Z path
4. Verify that no OS_ALIVE variable is leaked on any path
5. Verify that no OS_CONSUMED variable is used after consumption on any path
