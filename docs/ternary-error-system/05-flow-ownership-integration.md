# Part 5: Flow Ownership Integration

**Goal:** Connect the error handling system to Flow Ownership. The ownership solver must understand all three exit paths and verify safety on each.

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

This is a **5-value enum** (with the addition of OS_BORROWED below). It is NOT a trit. Five states don't fit three values. Use the enum as-is.

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
 * Same state → keep it.
 * Either consumed/dropped + the other isn't → MAYBE_CONSUMED (conflict).
 * ALIVE + BORROWED → BORROWED (conservative: treat as still lent).
 * Everything else → MAYBE_CONSUMED.
 */
static OwnershipState ownership_merge(OwnershipState a, OwnershipState b) {
    if (a == b) return a;

    bool a_gone = (a == OS_CONSUMED || a == OS_DROPPED);
    bool b_gone = (b == OS_CONSUMED || b == OS_DROPPED);

    /* One path consumed, other didn't → conflict */
    if (a_gone || b_gone) return OS_MAYBE_CONSUMED;

    /* ALIVE + BORROWED → conservatively BORROWED */
    if ((a == OS_ALIVE && b == OS_BORROWED) ||
        (a == OS_BORROWED && b == OS_ALIVE))
        return OS_BORROWED;

    return OS_MAYBE_CONSUMED;
}
```

This is plain enum logic. No trit operations. The merge function handles 5 states with straightforward comparisons.

---

## How the three exit paths affect ownership

The exit path is genuinely ternary (-1, 0, +1). But the ownership states it produces are from the 5-value enum. The exit path SELECTS which effects fire, and those effects produce ownership state transitions.

### Z path (normal return): standard ownership transfer
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    defer CleanupTemp();      // fires on Z and N paths
    conn := Connect(host)?;  // conn: OS_ALIVE

    result := Process(buf, conn);
    return ok(result);
    // Z path: defer runs, drops run for remaining alive vars
    // buf: passed to Process → OS_CONSUMED
    // conn: passed to Process → OS_CONSUMED
}
```

### N path (error return): errdefer cleanup
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // fires on N path only

    conn := Connect(host)?;  // If this fails: N path
    errdefer Close(conn);     // fires on N path only

    // When Connect fails at ?:
    // errdefer Close(conn) runs → conn: OS_CONSUMED
    // errdefer Free(buf) runs → buf: OS_CONSUMED
    // All resources cleaned up. No leaks.
}
```

### P path (restart recovery): ownership preserved
```
fn Example() Result<Output, Error> {
    buf := Alloc(1024)?;     // buf: OS_ALIVE
    errdefer Free(buf);       // would fire on N, does NOT fire on P

    data := ReadFile(path)    // If this fails:
        restart UseCache() {  //   restart provides replacement
            return cache.Get(path);
        };
    // After restart: data is OS_ALIVE (restart produced a value)
    // buf: still OS_ALIVE (errdefer did NOT run)

    Process(buf, data);
    return ok(result);
}
```

**Key insight:** On the P path, ownership state is the SAME as the Z path. The restart produced a replacement value, so the variable is alive. The errdefer didn't fire, so resources allocated before the restart point are still owned. The flow analysis doesn't need special P-path logic — it just needs to know that errdefer effects are inactive on the P path.

---

## Adding defer_trigger to FlowEffect

The flow analysis needs to know which effects come from errdefers (and therefore only fire on the N path).

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
    int8_t       defer_trigger;  /* -1=errdefer, 0=normal/defer, +1=recovery */
    FlowEffect  *next;
};
```

The `defer_trigger` field is an `int8_t` with three values — this IS genuinely ternary. It records which exit path this effect is active on. Normal code effects have `defer_trigger=0` (always active on N and Z). Errdefer effects have `defer_trigger=-1` (active on N only). Recovery-defer effects have `defer_trigger=+1` (active on P only).

When the flow analysis scans an ND_DEFER node to build effects, it sets this field:
```c
/* In flow.c — effect scanning for ND_DEFER */
FlowEffect *eff = create_effect(fa, EFF_CONSUME, var_id, node, loc);
if (node->kind == ND_DEFER && node->u.defer.is_errdefer)
    eff->defer_trigger = -1;   /* N: errdefer */
else
    eff->defer_trigger = 0;    /* Z: defer or normal code */
```

---

## Path-aware ownership propagation

**File:** `src/flow.c`

The current `ownership_propagate` function processes all effects unconditionally. With the ternary error system, some effects only fire on certain paths.

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

### After — filter by exit path:
```c
/*
 * Apply effects filtered by exit path.
 * Normal code effects (defer_trigger=0) always apply.
 * Errdefer effects (defer_trigger=-1) only apply when analyzing the N path.
 * Recovery effects (defer_trigger=+1) only apply when analyzing the P path.
 *
 * For the common case (no errdefers in the block), this is identical
 * to the old code — the trigger check is always true for trigger=0.
 */
for (FlowEffect *e = blk->effects; e; e = e->next) {
    /* Skip effects that don't fire on the current analysis path */
    if (e->defer_trigger == -1 && analysis_path != -1) continue;  /* errdefer on non-N path */
    if (e->defer_trigger == 1 && analysis_path != 1) continue;    /* recovery on non-P path */

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
- For functions that don't use errdefer or restarts: one pass with `analysis_path=0` (identical to current behavior)
- For functions with errdefer: two passes — `analysis_path=0` (Z) and `analysis_path=-1` (N)
- For functions with restarts: three passes — Z, N, and P

---

## Coupled solver integration

From the coupled-ownership-solver blog post — each solver level receives structured failure constraints. The exit path adds one field to the constraint:

```c
typedef struct {
    int          var_id;
    OwnershipState expected;    /* what state we expected */
    OwnershipState actual;      /* what state we found */
    SrcLoc       location;
    int8_t       on_path;       /* which exit path this conflict is on: -1, 0, +1 */
} OwnershipConflict;
```

L1 reports: "variable `buf` is OS_ALIVE at function exit on the N path — expected OS_CONSUMED (it should have been cleaned up by errdefer)."

L2 receives this and checks: is the errdefer reachable on the N path? Does it consume `buf`? If yes, L1 was wrong (the errdefer handles it). If no, it's a real leak.

The `on_path` field tells L2 which path to check. Without it, L2 would have to re-analyze all paths. With it, L2 targets the specific path that has the conflict.

---

## Verification

For each test program with errdefer or restarts:

1. Run ownership propagation on Z path — verify all resources owned or consumed
2. Run ownership propagation on N path — verify errdefers consume their targets
3. If restarts exist, run on P path — verify resources are same state as Z path
4. Verify no OS_ALIVE variables leak on any path
5. Verify no OS_CONSUMED variables are used after consumption on any path

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 768/768 + ternary defer tests
```
