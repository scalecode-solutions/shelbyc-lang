# Wake Pentit Design — Structured Concurrency with Composable Error Paths

**Date:** March 26, 2026
**Status:** Design — no code changes yet
**Depends on:** Composable error system (M1-M5), wake scheduler, channels

---

## The Core Insight

A wake's outcome is a pentit. The same truth table that governs defer cleanup governs nursery/supervisor decisions. One system, one firing rule, applied at both the function level and the concurrency level.

```
N2 — wake panicked        (unrecoverable: OOM, stack overflow, abort)
N1 — wake returned error  (clean failure: returned err(...), ? propagated)
Z  — wake is running      (normal: still alive, or exited with no Result)
P1 — wake completed ok    (success: returned ok(...) or void return)
P2 — wake recovered       (supervisor restarted it after failure)
```

---

## Three Resolutions — The Developer Picks What Fits

The same way the error system offers bool/trit/pentit resolution for defer cleanup, the concurrency system offers bool/trit/pentit resolution for wake observation. Nothing replaces anything. Everything composes.

### Bool level (2 outcomes) — "did it finish?"

Most code only needs to know: did the wakes finish, or are they still running. Fire-and-forget with a sync point.

```shelbyc
nursery {
    wake Fetch("url1", &data);
    wake Fetch("url2", &data);
}
// Done. Either they finished or we wouldn't be here.
// No error handling. No restart. Just "wait for all."
```

The nursery collapses pentit to bool: **running (Z)** or **done (not Z)**. The developer doesn't check outcomes, doesn't handle errors, doesn't supervise. Two states.

### Trit level (3 outcomes) — "did it succeed, fail, or recover?"

When you care about errors but don't need to distinguish panic from normal errors, or success from recovery.

```shelbyc
nursery {
    wake Fetch("url1", &data);
    wake Fetch("url2", &data);
}
// Trit outcomes from the nursery's perspective:
//   N — at least one wake failed (N2 or N1 collapsed to N)
//   Z — still running (shouldn't happen after nursery exits)
//   P — all wakes succeeded (P1 or P2 collapsed to P)

errdefer Println("some fetch failed");     // fires on N
successdefer Println("all fetches ok");     // fires on P
defer Println("fetches attempted");         // fires always
```

The nursery collapses pentit to trit: **failed (N)**, **running (Z)**, **succeeded (P)**. Three states. Errdefer and successdefer handle the N and P cases.

### Pentit level (5 outcomes) — full resolution

When you need to distinguish panic from error, success from recovery, and handle each differently.

```shelbyc
nursery {
    wake ProcessTransaction(tx1);
    wake ProcessTransaction(tx2);
}
// Pentit outcomes — the full picture:
//   N2 — a wake panicked (corrupted state, need emergency cleanup)
//   N1 — a wake returned err (transaction failed, need rollback)
//   Z  — still running
//   P1 — all wakes returned ok (transactions committed)
//   P2 — a wake was restarted by supervisor and eventually succeeded

panicdefer  EmergencyShutdown();    // N2 only
errdefer    RollbackAll();          // N1 and N2
defer       LogCompletion();        // always
successdefer CommitAll();           // P1 and P2
recoverdefer LogRecovery();         // P2 only
```

### The Resolution Tree

Each branch point uses the type that matches its cardinality. A nursery's outcome can feed into another nursery's decision, at any resolution level.

```
            nursery exit (pentit)
                    │
         ┌──────┬──┴──┬──────┬──────┐
         N2     N1    Z     P1     P2
         │      │           │      │
         │      └─────┐     └──┐   │
         │            │        │   │
         ▼            ▼        ▼   ▼
    panicdefer   errdefer  successdefer  recoverdefer
                                │
                    ┌───────────┴───────────┐
                    │                       │
              trit decision           bool decision
              (retry? skip?           (log? don't?)
               escalate?)
                    │
         ┌─────────┼─────────┐
         N         Z         P
         │                   │
    pentit decision    pentit decision
    (which error?      (which success
     classify it)       path to take)
         │
    ┌────┼────┐────┐────┐
    N2   N1   Z   P1   P2
    ...  ...      ...  ...
```

Each node picks the resolution that fits. The nursery produces pentit. The errdefer handler might only need trit (error/normal/recovered). The retry logic inside might just need bool (retry or give up). They compose downward. A pentit decision can branch into trits, which can branch into bools. Or a pentit can branch directly into another pentit. The tree is heterogeneous — each node uses the type that matches its branching factor.

---

## The Three Constructs

### 1. `nursery { }` — Structured concurrency scope

All wakes spawned inside a nursery are tracked. The nursery doesn't exit until every wake finishes (or is cancelled). This is the structured concurrency guarantee: no orphan wakes, no dangling references to stack variables.

```shelbyc
fn Main() {
    data := Vec<i32>.New();

    nursery {
        wake Fetch("url1", &data);
        wake Fetch("url2", &data);
        wake Fetch("url3", &data);
    }
    // ALL three wakes finished (or failed) before we get here.
    // &data references are guaranteed valid for the lifetime of the nursery.
    Println("fetched {data.Len()} items");
}
```

**Nursery semantics:**
- Wakes spawned inside are children of the nursery
- Nursery blocks until all children reach a terminal state (N2, N1, P1, or P2)
- If any child hits N1 or N2, the nursery can cancel remaining children
- The nursery's own exit path is the "worst" pentit of its children

**Nursery exit path computation:**
```
nursery_exit = min(child_exit for each child)
```
If all children are P1 (success), nursery exits P1. If any child is N1 (error), nursery exits N1. If any child is N2 (panic), nursery exits N2. The min() on pentit raw encoding gives the most severe outcome.

### 2. `supervisor { }` — Restart failed wakes

A supervisor wraps a nursery (or a single wake) and provides restart policies. When a child fails (N1 or N2), the supervisor can restart it instead of propagating the failure.

```shelbyc
fn Main() {
    supervisor(max_restarts: 3) {
        wake UnreliableWorker(job);
    }
    // If UnreliableWorker fails, supervisor restarts it (up to 3 times).
    // If it succeeds after restart, exit path is P2 (recovered).
    // If it fails 3 times, exit path is N1 (error, max restarts exceeded).
}
```

**Supervisor semantics:**
- Wraps a nursery with restart policy
- On child N1: restart the child (P2 path for the child)
- On child N2: depends on policy (restart or propagate)
- Tracks restart count per child
- Exceeding max_restarts → propagate the error (N1 for supervisor)

### 3. `wake` with pentit outcome — The outcome channel

Every wake spawned in a nursery has an implicit "outcome" that the nursery observes. The wake doesn't need to do anything special — its return type determines the outcome:

```shelbyc
// Void return → P1 on normal exit, N2 on panic
fn Worker() {
    DoWork();
    // implicit P1 (success) — reached the end
}

// Result return → P1 on ok, N1 on err
fn FallibleWorker() Result<i32, str> {
    data := Fetch("url")?;   // N1 if Fetch fails
    return ok(data);          // P1 on success
}
```

**Outcome mapping from return type:**

| Return type | Normal exit | Error exit | Panic |
|------------|-------------|------------|-------|
| void | P1 | — | N2 |
| i32 (non-Result) | P1 | — | N2 |
| Result<T, E> | P1 (ok) | N1 (err) | N2 |

---

## The Pentit Wake State Machine

```
             spawn
               │
               ▼
           ┌───────┐
           │  Z(2)  │◄──────────────────────┐
           │running │                        │
           └───┬───┘                    restart (P2)
               │                             │
        ┌──────┼──────┐               ┌──────┴──────┐
        │      │      │               │  supervisor │
        ▼      ▼      ▼               │  decision   │
    ┌──────┐┌──────┐┌──────┐          └─────────────┘
    │ N2(0)││ N1(1)││ P1(3)│                 ▲
    │panic ││error ││  ok  │                 │
    └──────┘└──┬───┘└──────┘                 │
               │                             │
               └─────────────────────────────┘
                    (if supervised)
```

---

## C Bootstrap Implementation

### New AST Nodes

```c
ND_NURSERY,      /* nursery { wake ...; wake ...; } */
ND_SUPERVISOR,   /* supervisor(opts) { wake ...; } */
```

### New union members in ast.h

```c
/* ND_NURSERY: structured concurrency scope */
struct {
    AstNode *body;           /* block containing wake statements */
    AstNode *cancel_policy;  /* optional: cancel on first error? */
} nursery;

/* ND_SUPERVISOR: restart policy wrapper */
struct {
    AstNode *body;           /* block (usually containing a nursery) */
    int      max_restarts;   /* -1 = unlimited */
} supervisor;
```

### New Runtime Types

```c
/* Wake outcome — pentit encoding of how a wake finished */
typedef struct {
    int8_t   exit_path;    /* pentit: 0=N2, 1=N1, 2=Z(running), 3=P1, 4=P2 */
    void    *error_value;  /* non-NULL for N1 (the error data) */
} WakeOutcome;

/* Nursery — tracks a group of wakes */
typedef struct ScNursery {
    ScW           **children;      /* array of child wakes */
    WakeOutcome    *outcomes;      /* outcome per child */
    int             child_count;
    int             child_cap;
    int             completed;     /* how many have finished */
    pthread_mutex_t mu;
    pthread_cond_t  done_cond;     /* signaled when a child finishes */
    bool            cancel_on_error;  /* cancel remaining on first N1/N2? */
    int8_t          nursery_exit;  /* worst exit path seen */
} ScNursery;
```

### __sc_wake_exit Integration

When a wake exits, it reports its outcome to the nursery:

```c
/* In runtime_sched.c — called by __sc_context_entry when fn() returns */
void __sc_wake_exit(void) {
    ScW *gp = sc_m->curw;

    /* Determine exit path from the wake's return value */
    int8_t exit_path = 3;  /* P1: default for void/non-Result return */

    if (gp->fn_result_ptr) {
        /* Wake function returned a Result — check is_ok */
        bool is_ok = *(bool *)gp->fn_result_ptr;
        exit_path = is_ok ? 3 : 1;  /* P1 or N1 */
    }

    /* Report to nursery if we're in one */
    if (gp->nursery) {
        ScNursery *n = gp->nursery;
        pthread_mutex_lock(&n->mu);

        /* Find our slot and record outcome */
        for (int i = 0; i < n->child_count; i++) {
            if (n->children[i] == gp) {
                n->outcomes[i].exit_path = exit_path;
                n->outcomes[i].error_value = (exit_path == 1) ?
                    (void *)((char *)gp->fn_result_ptr + sizeof(bool)) : NULL;

                /* Update nursery's worst exit */
                if (exit_path < n->nursery_exit)
                    n->nursery_exit = exit_path;

                n->completed++;
                break;
            }
        }

        /* Cancel remaining if policy says so */
        if (n->cancel_on_error && exit_path <= 1) {
            for (int i = 0; i < n->child_count; i++) {
                if (n->outcomes[i].exit_path == 2) {  /* still Z (running) */
                    /* Request cancellation — set preempt flag */
                    atomic_store_explicit(&n->children[i]->preempt,
                                          true, memory_order_release);
                }
            }
        }

        /* Signal nursery that a child finished */
        pthread_cond_signal(&n->done_cond);
        pthread_mutex_unlock(&n->mu);
    }

    /* Mark wake as dead and return to scheduler */
    gp->switch_reason = SWITCH_EXIT;
    __sc_context_switch(&gp->sched, &sc_m->w0->sched);
    /* unreachable */
}
```

### __sc_nursery_wait — The nursery blocks until all children finish

```c
/* Called by the nursery's parent wake. Blocks until all children
 * have a terminal exit path (not Z). Returns the nursery's exit path. */
int8_t __sc_nursery_wait(ScNursery *n) {
    pthread_mutex_lock(&n->mu);
    while (n->completed < n->child_count) {
        pthread_cond_wait(&n->done_cond, &n->mu);
    }
    int8_t result = n->nursery_exit;
    pthread_mutex_unlock(&n->mu);
    return result;
}
```

### Nursery Codegen

The `nursery { ... }` block generates:

```c
/* Codegen for ND_NURSERY */
case ND_NURSERY: {
    /* 1. Allocate and initialize ScNursery */
    LLVMValueRef nursery = call __sc_nursery_new(cancel_on_error);

    /* 2. Set nursery as current nursery in codegen context */
    cg->current_nursery = nursery;

    /* 3. Emit the body — wake statements inside will register
     *    children with the nursery */
    cg_expr(cg, node->u.nursery.body);

    /* 4. Wait for all children */
    LLVMValueRef exit_path = call __sc_nursery_wait(nursery);

    /* 5. Run cleanup based on exit path */
    cg_emit_all_scope_cleanup_pentit(cg, exit_path);

    /* 6. Destroy nursery */
    call __sc_nursery_destroy(nursery);

    cg->current_nursery = saved_nursery;
    break;
}
```

### Wake Spawn Inside Nursery

When `wake Fn(args)` is inside a nursery, the spawn registers the new wake as a child:

```c
/* Modified __sc_wake_spawn for nursery-aware spawning */
void __sc_wake_spawn(void (*fn)(void *), void *arg) {
    /* ... existing spawn code ... */

    /* If there's a current nursery, register this wake as a child */
    ScNursery *n = sc_m->curw->nursery_scope;  /* set by nursery codegen */
    if (n) {
        gp->nursery = n;
        pthread_mutex_lock(&n->mu);
        if (n->child_count >= n->child_cap) {
            n->child_cap = n->child_cap < 16 ? 16 : n->child_cap * 2;
            n->children = realloc(n->children, sizeof(ScW *) * n->child_cap);
            n->outcomes = realloc(n->outcomes, sizeof(WakeOutcome) * n->child_cap);
        }
        n->outcomes[n->child_count] = (WakeOutcome){ .exit_path = 2, .error_value = NULL };
        n->children[n->child_count] = gp;
        n->child_count++;
        pthread_mutex_unlock(&n->mu);
    }

    /* ... rest of spawn ... */
}
```

---

## Supervisor as Handle/Restart at the Wake Level

The supervisor IS handle/restart. The handle block wraps the nursery. The restart is "spawn the wake again."

```shelbyc
fn ProcessJob(job Job) Result<Output, Error> {
    data := Fetch(job.url)?;
    return ok(Transform(data));
}

fn Main() {
    jobs := GetJobs();

    nursery {
        for j in jobs {
            // Each wake runs inside the nursery.
            // If it fails, the supervisor restarts it.
            wake handle ProcessJob(j) {
                on _ => invoke Retry
            }
                restart Retry() { ProcessJob(j) };
        }
    }
    // All jobs completed (some may have been retried).
    // Nursery exit path: P1 if all succeeded, P2 if any were retried,
    // N1 if any failed after max retries.
}
```

The `handle`/`restart` mechanism we already built works here. The `__sc_restart_choice` thread-local becomes wake-local (stored on ScW instead of TLS). When a wake fails and a supervisor is watching, the supervisor sets the restart choice and re-spawns the wake.

---

## Defer Integration

The nursery exit path flows directly into the defer system:

```shelbyc
fn Pipeline() {
    defer Println("pipeline done");           // fires always
    errdefer Println("pipeline had errors");   // fires if any wake failed
    successdefer Println("all wakes ok");      // fires if all succeeded

    nursery {
        wake StepA();
        wake StepB();
        wake StepC();
    }
    // nursery_exit feeds into cg_emit_all_scope_cleanup_pentit
    // If nursery_exit == P1: successdefer fires, errdefer skips
    // If nursery_exit == N1: errdefer fires, successdefer skips
    // defer fires always
}
```

The pentit truth table we built for function-level cleanup applies unchanged at the concurrency level. No new mechanism needed — just a new source of exit paths.

---

## The Multi-Channel Deadlock Fix

The current scheduler deadlocks because parked M's aren't woken when work becomes available. The nursery design reveals why: without structured concurrency, there's no way to know that 100 collectors are all waiting and 100,000 workers have all sent. The scheduler just sees parked wakes and idle M's.

The fixes are:

### 1. Nursery-aware scheduling

When a nursery is active, the scheduler knows exactly how many wakes are outstanding and can prioritize them:

```c
/* In findrunnable — if we're in a nursery, prioritize nursery children */
if (mp->curw && mp->curw->nursery) {
    ScNursery *n = mp->curw->nursery;
    /* Check if any nursery children are runnable on our queue */
    /* This prevents non-nursery work from starving nursery completion */
}
```

### 2. Bounded concurrency in nurseries

```shelbyc
nursery(max_concurrent: 10) {
    for i := 0; i < 100000; i++ {
        wake Worker(i, ch);  // only 10 run at a time
    }
}
```

This prevents spawning 100,000 simultaneous wakes. Instead, the nursery maintains a semaphore — when 10 are running, new spawns block until a slot opens. This solves the stack exhaustion AND the scheduling starvation.

### 3. Work-group aware scheduling

Wakes in the same nursery form a work group. The scheduler can batch schedule them — when one wake in a group parks, immediately switch to another wake in the same group rather than doing a full `findrunnable` scan.

```c
/* In execute — after a wake parks, check if siblings are runnable */
if (gp->nursery) {
    ScNursery *n = gp->nursery;
    for (int i = 0; i < n->child_count; i++) {
        if (atomic_load(&n->children[i]->status) == W_RUNNABLE) {
            /* Found a sibling — run it immediately */
            return n->children[i];
        }
    }
}
/* Fallback to normal findrunnable */
```

---

## Implementation Order

| Phase | What | Files | Tests |
|-------|------|-------|-------|
| 1 | `ScNursery` struct + `__sc_nursery_new/wait/destroy` runtime functions | runtime_sched.h, runtime_sched.c | Unit: nursery lifecycle |
| 2 | `WakeOutcome` reporting in `__sc_wake_exit` | runtime_sched.c | Unit: wake outcome pentit |
| 3 | `nursery { }` keyword, parser, AST node | token.h, lexer.c, ast.h, parser.c | Parse test |
| 4 | Nursery codegen — spawn registration, wait, exit path | cg_stmt.c, cg_literal.c | Gate: basic nursery |
| 5 | Nursery exit path → defer cleanup integration | cg_stmt.c | Gate: nursery + errdefer |
| 6 | Cancel on error policy | runtime_sched.c | Gate: nursery cancel |
| 7 | `max_concurrent` bounded spawning | runtime_sched.c | Gate: bounded nursery |
| 8 | Supervisor as handle/restart on nursery | parser.c, cg_literal.c | Gate: supervised wake |
| 9 | Work-group scheduling optimization | runtime_sched.c | Stress: multi-channel |
| 10 | Fix the multi-channel deadlock | runtime_sched.c | 100k multi-channel test |

---

## What This Solves

1. **No orphan wakes** — nursery guarantees all children finish before scope exits
2. **Stack reference safety** — borrows to stack variables are valid for nursery lifetime
3. **Structured error propagation** — worst-child pentit flows into defer system
4. **Automatic cleanup** — errdefer at nursery level handles wake failures
5. **Bounded concurrency** — max_concurrent prevents resource exhaustion
6. **Restart policies** — supervisor reuses handle/restart mechanism
7. **Multi-channel deadlock** — work-group scheduling prevents starvation
8. **Composability** — same pentit truth table, same defer keywords, same firing rule

Everything builds off everything else. The pentit wake outcome feeds the defer truth table. The nursery uses the handle/restart mechanism. The supervisor IS conditions/restarts at the concurrency level. Bit, trit, pentit — the developer picks the resolution that fits.

---

*The wake scheduler doesn't need a separate health-check system. It needs to produce pentit exit paths and flow them into the defer system that already exists. The composable error system IS the concurrency error system. One mechanism, applied at every level.*
