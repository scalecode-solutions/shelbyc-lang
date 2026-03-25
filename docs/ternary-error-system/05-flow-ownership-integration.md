# Part 5: Flow Ownership Integration

**Goal:** Connect the error handling system to Flow Ownership. The ownership solver must understand all five exit paths and verify safety on each.

**Files modified:** `flow.h`, `flow.c`

**Depends on:** Parts 1-4 (all five paths functional)

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

This is a **5-value enum** (with the addition of OS_BORROWED below). It was already pentit-cardinality, just using an enum instead of a raw pentit. Five ownership states. Five defer triggers. The cardinality matches — not by accident.

---

## Add OS_BORROWED

OwnershipState currently has no representation for "temporarily lent." A variable that's borrowed isn't fully alive (can't be moved) and isn't consumed (will come back). This is a distinct state the solver needs to track.

### After:
```c
typedef enum {
    OS_ALIVE,           /* value exists and is usable */
    OS_BORROWED,        /* temporarily lent via & — can't move, will return */
    OS_CONSUMED,        /* value has been moved away */
    OS_MAYBE_CONSUMED,  /* consumed on some paths, alive on others (merge point) */
    OS_DROPPED,         /* value has been explicitly dropped */
} OwnershipState;
```

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
 * Same state -> keep it.
 * Either consumed/dropped + the other isn't -> MAYBE_CONSUMED (conflict).
 * ALIVE + BORROWED -> BORROWED (conservative: treat as still lent).
 * Everything else -> MAYBE_CONSUMED.
 */
static OwnershipState ownership_merge(OwnershipState a, OwnershipState b) {
    if (a == b) return a;

    bool a_gone = (a == OS_CONSUMED || a == OS_DROPPED);
    bool b_gone = (b == OS_CONSUMED || b == OS_DROPPED);

    /* One path consumed, other didn't -> conflict */
    if (a_gone || b_gone) return OS_MAYBE_CONSUMED;

    /* ALIVE + BORROWED -> conservatively BORROWED */
    if ((a == OS_ALIVE && b == OS_BORROWED) ||
        (a == OS_BORROWED && b == OS_ALIVE))
        return OS_BORROWED;

    return OS_MAYBE_CONSUMED;
}
```

This is plain enum logic. No pentit operations. The merge function handles 5 states with straightforward comparisons.

---

## How the five exit paths affect ownership

The exit path is a pentit (i8, values 0-4). But the ownership states it produces are from the 5-value enum. The exit path SELECTS which effects fire, and those effects produce ownership state transitions.

### Z path (normal return): standard ownership transfer
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    defer CleanupTemp();      // fires on ALL paths (trigger=2, Z)
    conn := Connect(host)?;  // conn: OS_ALIVE

    result := Process(buf, conn);
    return ok(result);
    // Z path (exit_path=2): defer runs, drops run for remaining alive vars
    // buf: passed to Process -> OS_CONSUMED
    // conn: passed to Process -> OS_CONSUMED
}
```

### N1 path (error return): errdefer cleanup
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // fires on N1 and N2 paths (trigger=1)

    conn := Connect(host)?;  // If this fails: N1 path
    errdefer Close(conn);     // fires on N1 and N2 paths (trigger=1)

    // When Connect fails at ?:
    // errdefer Close(conn) runs -> conn: OS_CONSUMED
    // errdefer Free(buf) runs -> buf: OS_CONSUMED
    // All resources cleaned up. No leaks.
}
```

### N2 path (panic): panicdefer cleanup
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;          // buf: OS_ALIVE
    panicdefer CoreDump(buf);      // fires ONLY on N2 (trigger=0)
    errdefer Free(buf);            // fires on N1 AND N2 (trigger=1)
    defer Log("exiting");          // fires on ALL paths (trigger=2, Z)

    // On panic (exit_path=0):
    //   panicdefer CoreDump(buf) runs  (0 <= 0)
    //   errdefer Free(buf) runs        (0 <= 1)
    //   defer Log("exiting") runs      (Z always fires)
    // On error (exit_path=1):
    //   panicdefer CoreDump(buf) SKIPPED (1 is NOT <= 0)
    //   errdefer Free(buf) runs          (1 <= 1)
    //   defer Log("exiting") runs        (Z always fires)
}
```

### P2 path (restart recovery): ownership preserved
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // would fire on N1/N2, does NOT fire on P2
    recoverdefer Log("recovered");  // fires ONLY on P2 (trigger=4)

    data := ReadFile(path)    // If this fails:
        restart UseCache() {  //   restart provides replacement
            return cache.Get(path);
        };
    // After restart: data is OS_ALIVE (restart produced a value)
    // buf: still OS_ALIVE (errdefer did NOT run — exit_path=4, 4 <= 1 is false)
    // recoverdefer fired (4 >= 4)

    Process(buf, data);
    return ok(result);
}
```

### P1 path (success): successdefer fires
```
fn Example() Result<Output, Error> {
    successdefer CommitTransaction();  // fires on P1 and P2 (trigger=3)
    errdefer RollbackTransaction();    // fires on N1 and N2 (trigger=1)

    // On success path (exit_path=3):
    //   successdefer CommitTransaction() runs  (3 >= 3)
    //   errdefer RollbackTransaction() SKIPPED (3 is NOT <= 1)
    // On error path (exit_path=1):
    //   successdefer CommitTransaction() SKIPPED (1 is NOT >= 3)
    //   errdefer RollbackTransaction() runs      (1 <= 1)
}
```

**Key insight:** On the P paths, ownership state depends on which defers fired. The flow analysis doesn't need special P-path logic — it just needs to know that N-side defers are inactive on P paths, and P-side defers are inactive on N paths. The pentit firing rule in `cg_emit_scope_defers_pentit` handles this automatically.

---

## Adding defer_trigger to FlowEffect

The flow analysis needs to know which effects come from defers of each type (and therefore only fire on certain paths).

**File:** `src/flow.h` — extend FlowEffect:

### Before:
```c
struct FlowEffect {
    EffectKind   kind;
    int          var_id;
    AstNode     *node;
    SrcLoc       loc;
    FlowEffect  *next;
};
```

### After:
```c
struct FlowEffect {
    EffectKind   kind;
    int          var_id;
    AstNode     *node;
    SrcLoc       loc;
    int8_t       defer_trigger;  /* pentit raw encoding: 0=N2, 1=N1, 2=Z, 3=P1, 4=P2 */
    FlowEffect  *next;
};
```

The `defer_trigger` field is an `int8_t` with five values — a pentit. It records which exit path this effect is active on. Normal code effects and `defer` effects have `defer_trigger=2` (Z — always active). Errdefer effects have `defer_trigger=1` (N1 — active on error and panic). Panicdefer effects have `defer_trigger=0` (N2 — active on panic only). Successdefer effects have `defer_trigger=3` (P1 — active on success and recovery). Recoverdefer effects have `defer_trigger=4` (P2 — active on recovery only).

When the flow analysis scans an ND_DEFER node to build effects, it sets this field:
```c
/* In flow.c — effect scanning for ND_DEFER */
FlowEffect *eff = create_effect(fa, EFF_CONSUME, var_id, node, loc);
if (node->kind == ND_DEFER)
    eff->defer_trigger = node->u.defer.trigger;  /* pentit value from AST */
else
    eff->defer_trigger = 2;  /* Z: normal code, always active */
```

---

## Path-aware ownership propagation

**File:** `src/flow.c`

The current `ownership_propagate` function processes all effects unconditionally. With the pentit error system, some effects only fire on certain paths.

### Current (flow.c:1036-1055):
```c
for (FlowEffect *e = blk->effects; e; e = e->next) {
    int vid = e->var_id;
    switch (e->kind) {
    case EFF_WRITE:   local[vid] = OS_ALIVE; break;
    case EFF_CONSUME: local[vid] = OS_CONSUMED; break;
    case EFF_DROP:    local[vid] = OS_DROPPED; break;
    case EFF_READ:
    case EFF_BORROW:
    case EFF_BORROW_MUT:
        break;
    }
}
```

### After — filter by exit path using pentit firing rule:
```c
/*
 * Apply effects filtered by exit path using the same pentit firing rule
 * as cg_emit_scope_defers_pentit:
 *
 *   N-side triggers (0, 1): fire if exit_path <= trigger
 *   Z trigger (2): always fire
 *   P-side triggers (3, 4): fire if exit_path >= trigger
 *
 * For the common case (no errdefers in the block), this is identical
 * to the old code — the trigger check is always true for trigger=2.
 */
for (FlowEffect *e = blk->effects; e; e = e->next) {
    /* Apply pentit firing rule to filter effects by analysis path */
    bool should_apply;
    if (e->defer_trigger == 2) {
        should_apply = true;                                 /* Z: always */
    } else if (e->defer_trigger < 2) {
        should_apply = (analysis_path <= e->defer_trigger);  /* N side */
    } else {
        should_apply = (analysis_path >= e->defer_trigger);  /* P side */
    }
    if (!should_apply) continue;

    int vid = e->var_id;
    switch (e->kind) {
    case EFF_WRITE:   local[vid] = OS_ALIVE; break;
    case EFF_CONSUME: local[vid] = OS_CONSUMED; break;
    case EFF_DROP:    local[vid] = OS_DROPPED; break;
    case EFF_BORROW:
    case EFF_BORROW_MUT:
        local[vid] = OS_BORROWED; break;  /* NEW: track borrow state */
    case EFF_READ:
        break;
    }
}
```

The `analysis_path` is passed into the propagation function. The solver runs propagation once per relevant path:
- For functions that use only `defer`: one pass with `analysis_path=2` (Z, identical to current behavior)
- For functions with `errdefer`: two passes — `analysis_path=2` (Z) and `analysis_path=1` (N1)
- For functions with `panicdefer`: three passes — Z, N1, and N2 (`analysis_path=0`)
- For functions with `successdefer`: add `analysis_path=3` (P1)
- For functions with `recoverdefer` / restarts: add `analysis_path=4` (P2)
- Maximum: five passes (one per pentit value) for functions that use all five defer types

In practice, most functions need 1-2 passes. The solver scans the defer stack to determine which trigger values are present and only runs passes for those paths.

---

## Coupled solver integration

From the coupled-ownership-solver blog post — each solver level receives structured failure constraints. The exit path adds one field to the constraint:

```c
typedef struct {
    int          var_id;
    OwnershipState expected;    /* what state we expected */
    OwnershipState actual;      /* what state we found */
    SrcLoc       location;
    int8_t       on_path;       /* pentit: which exit path this conflict is on (0-4) */
} OwnershipConflict;
```

L1 reports: "variable `buf` is OS_ALIVE at function exit on the N1 path — expected OS_CONSUMED (it should have been cleaned up by errdefer)."

L2 receives this and checks: is the errdefer reachable on the N1 path? Does it consume `buf`? If yes, L1 was wrong (the errdefer handles it). If no, it's a real leak.

The `on_path` field tells L2 which path to check. Without it, L2 would have to re-analyze all paths. With it, L2 targets the specific path that has the conflict.

**Path values in diagnostics:**

| on_path | Meaning | Typical conflict |
|---------|---------|-----------------|
| 0 (N2) | Panic path | Resource not freed by panicdefer |
| 1 (N1) | Error path | Resource not freed by errdefer |
| 2 (Z) | Normal path | Resource leaked on normal exit |
| 3 (P1) | Success path | Transaction not committed by successdefer |
| 4 (P2) | Recovery path | Resource ownership wrong after restart |

---

## Verification

For each test program with any non-Z defer or restarts:

1. Run ownership propagation on Z path (analysis_path=2) — verify all resources owned or consumed
2. Run ownership propagation on N1 path (analysis_path=1) — verify errdefers consume their targets
3. Run ownership propagation on N2 path (analysis_path=0) — verify panicdefers handle their targets
4. If successdefer present, run on P1 path (analysis_path=3) — verify success-only effects
5. If restarts present, run on P2 path (analysis_path=4) — verify resources are same state as Z path
6. Verify no OS_ALIVE variables leak on any analyzed path
7. Verify no OS_CONSUMED variables are used after consumption on any path

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 772/772 + pentit defer tests
```
