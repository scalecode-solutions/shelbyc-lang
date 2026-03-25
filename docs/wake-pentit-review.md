# Wake Pentit Design — Review and Issues

Review of `wake-pentit-design.md`. The architecture is sound. These are issues that need addressing before or during implementation.

---

## Issue 1: `min()` Crew Exit Is Too Aggressive

**Problem:** If 99 wakes succeed (P1) and 1 returns error (N1), the crew exit is N1. Every `errdefer` fires. Every `successdefer` is skipped. One flaky network call kills the entire pipeline's success path.

Real systems need partial success. A batch job where 99% of items processed correctly shouldn't trigger full error cleanup.

**Suggestion:** Crew exit policies instead of hardcoded `min()`:

```shelbyc
crew {                              // default: AllSucceed (current min behavior)
    wake Worker(job);
}

crew(policy: BestEffort) {          // crew always exits P1, failures collected
    wake Worker(job);
}

crew(policy: Majority) {            // P1 if >50% succeeded
    wake Worker(job);
}

crew(policy: Threshold(0.95)) {     // P1 if >=95% succeeded
    wake Worker(job);
}
```

The crew should also expose individual outcomes for inspection:

```shelbyc
n := crew {
    wake Worker(job1);
    wake Worker(job2);
    wake Worker(job3);
}
// n.Outcomes() returns []WakeOutcome
// n.SuccessCount(), n.FailCount(), n.ExitPath()
// Developer decides what partial success means for their use case
```

`AllSucceed` (min) should remain the default — it's the safe choice. Other policies are opt-in for cases where partial success is acceptable.

---

## Issue 2: Cancel-on-Error Races with In-Flight Work

**Problem:** When a child hits N1 and the crew sets `preempt = true` on siblings, those siblings might be:
- Holding a mutex
- Mid-write to a channel
- Halfway through a multi-step operation
- Inside a defer cleanup chain

The `preempt` flag in the scheduler is for cooperative preemption (yield at function call boundaries). Using it for cancellation mixes two different concerns. A wake that's preempted at an arbitrary function call boundary might leave shared state inconsistent.

**Suggestion:** Cancellation should be cooperative, not preemptive. Add a `Cancelled()` builtin that wakes check at safe points:

```shelbyc
fn Worker(data &Data) Result<Output, Error> {
    for item in data.Items() {
        if Cancelled() { return err("cancelled"); }  // safe cancellation point
        Process(item)?;
    }
    return ok(result);
}
```

The crew sets a cancellation flag (not the preempt flag). The wake checks it when convenient. If the wake never checks, it runs to completion — which is safer than force-killing it mid-operation.

The `Cancelled()` check returns the error through the normal Result path, which means errdefer fires, cleanup happens, and the wake exits cleanly with N1. The crew sees the cancellation as an N1 exit, not an N2 panic.

For wakes that truly won't cooperate (infinite loop, deadlocked), a timeout (see Issue 7) is the fallback.

**Runtime change:**
```c
/* On ScW struct */
atomic_bool cancel_requested;  /* set by crew, read by Cancelled() builtin */

/* Cancelled() builtin codegen */
bool __sc_wake_cancelled(void) {
    return atomic_load_explicit(&sc_m->curw->cancel_requested, memory_order_acquire);
}
```

---

## Issue 3: Borrow Safety Across Crew Boundaries

**Problem:** The crew guarantees lifetime (references valid until crew exits) but not exclusivity. Two wakes with borrows to the same data can race:

```shelbyc
crew {
    wake Fetch("url1", &data);   // reads data
    wake Mutate(&mut data);      // writes data — DATA RACE
}
```

Even with shared `&` borrows, if the underlying type has interior mutability (Vec.Push modifies the Vec), shared borrows can race:

```shelbyc
crew {
    wake { data.Push(1); }   // &mut through method call
    wake { data.Push(2); }   // &mut through method call — RACE
}
```

**Suggestion:** Flow ownership should enforce at the crew level:
- `&mut` borrows: only ONE wake can hold a `&mut` to a given variable inside a crew. Multiple `&mut` wakes to the same variable is a compile error.
- `&` borrows: allowed for multiple wakes only if the type is proven immutable or the borrows are read-only. If any method called through the borrow is `&mut self`, the borrow is effectively `&mut` and falls under the single-wake rule.
- Channels as the escape hatch: if you need multiple wakes writing to the same data, pass a channel. The channel serializes access. This is what the Dining Philosophers solution already does.

```shelbyc
// COMPILE ERROR: two &mut borrows in same crew
crew {
    wake Mutate(&mut data);
    wake Mutate(&mut data);
}

// OK: channel serializes access
ch := chan<Item>.New(100);
crew {
    wake Producer(ch);
    wake Producer(ch);    // channels are designed for multi-sender
}
```

This is a flow ownership check at the crew scope level — the checker scans all wake spawn sites in a crew and verifies no aliasing violations. Same rule as function-level borrowing, applied to concurrent scope.

---

## Issue 4: Supervisor Restart with Stale Closure State

**Problem:** When a supervisor restarts a failed wake, what state does the closure see? If the wake captured `&mut data` and modified it before failing, does the restart see the original state or the modified state?

```shelbyc
counter := 0;
supervisor(max_restarts: 3) {
    wake {
        counter++;          // modifies captured state
        if counter < 3 {
            return err("not ready");  // fail, supervisor restarts
        }
        return ok(counter);
    }
}
// What is counter? 0? 1? 3?
```

If restart reuses the closure environment, `counter` accumulates across restarts (1, 2, 3 → succeeds on third try). If restart re-captures, `counter` resets to 0 each time (infinite restart loop).

**Suggestion:** Define the semantics explicitly:
- **Move captures** are re-captured on restart (the supervisor re-evaluates the spawn expression). This gives a clean restart from the original state. This is the Erlang model — restart from known-good state.
- **Borrow captures** see the current state of the borrowed variable (since the variable lives in the parent scope and wasn't moved). This allows accumulation across restarts when intentional.
- Document this clearly: "Supervisor restarts re-evaluate the wake spawn expression. Move-captured values are fresh. Borrow-captured values reflect mutations from previous attempts."

Alternatively, require that supervised wakes take parameters instead of capturing:

```shelbyc
fn Worker(job Job) Result<Output, Error> {
    // no captures — all state comes through parameters
    // supervisor re-calls Worker(job) with the same job
}

supervisor(max_restarts: 3) {
    wake Worker(job);   // restart = call Worker(job) again
}
```

This is cleaner and avoids the capture ambiguity entirely. Supervised wakes should probably be required to be named function calls, not closures.

---

## Issue 5: Z (Running) as a Pentit Value Is Overloaded

**Problem:** In the defer system, Z (2) means "fire always — the center, the universal path." In the wake system, Z (2) means "still running — not yet terminal." Same raw value, different semantics.

A crew waits until all children are NOT Z. But if someone accidentally uses the crew exit path (which could momentarily be Z during execution) as a defer trigger, Z means "always fire" in the defer context. The two meanings of Z collide.

**Suggestion:** Two options:

**Option A:** Accept the dual meaning and document it clearly. Z is "neutral/center/default" in both contexts. For defers, neutral means "always fire." For wakes, neutral means "still in progress." The crew never exposes Z as a final exit path — `__sc_crew_wait` blocks until Z is gone. So the collision never happens in practice.

**Option B:** Use a separate enum for wake state instead of reusing the pentit. WakeState has 5 values that happen to correspond to pentit values, but they're a separate type. The crew converts WakeState to pentit when computing its exit path for the defer system. This makes the code explicit about the conversion point.

Option A is probably fine. The crew guarantees Z never escapes as a terminal exit path. But the documentation should call out the dual meaning.

---

## Issue 6: Work-Group Scheduling Is O(N) Per Context Switch

**Problem:** When a wake parks inside a crew, the scheduler scans all crew children to find a runnable sibling:

```c
for (int i = 0; i < n->child_count; i++) {
    if (atomic_load(&n->children[i]->status) == W_RUNNABLE) {
```

With bounded concurrency (`max_concurrent: 10`), this is fine — scan 10 entries. But the tracking array grows to `child_count` which could be 100,000 if 100,000 wakes were spawned (bounded concurrency limits concurrent execution, not total spawns). Scanning 100,000 entries per context switch is too slow.

**Suggestion:** Each crew should have its own local run queue (FIFO ring buffer, same as the per-P run queue in the scheduler):

```c
typedef struct ScCrew {
    /* ... existing fields ... */

    /* Local run queue for crew children */
    ScW        *local_runq[256];   /* ring buffer */
    uint32_t    runq_head;
    uint32_t    runq_tail;
} ScCrew;
```

When a crew child becomes runnable (unparked from channel recv, newly spawned), it goes into the crew's local queue. When a wake in the crew parks, the scheduler checks the crew's local queue first — O(1) dequeue instead of O(N) scan.

Falls back to the normal per-P queue if the crew queue is empty.

---

## Issue 7: No Timeout on Crew Wait

**Problem:** `__sc_crew_wait` blocks forever if a child wake deadlocks or enters an infinite loop. The Dining Philosophers segfault scenario would hang here indefinitely instead of crashing. No way to recover.

```c
/* Current: blocks forever */
while (n->completed < n->child_count) {
    pthread_cond_wait(&n->done_cond, &n->mu);
}
```

**Suggestion:** Add optional timeout to nurseries:

```shelbyc
crew(timeout: 5000) {       // 5 second timeout in milliseconds
    wake SlowWorker();
}
// If timeout expires: remaining running wakes are cancelled (cooperative),
// crew exit path = N1 with a TimeoutError
```

Runtime implementation:

```c
int8_t __sc_crew_wait(ScCrew *n) {
    pthread_mutex_lock(&n->mu);

    if (n->timeout_ms > 0) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += n->timeout_ms / 1000;
        deadline.tv_nsec += (n->timeout_ms % 1000) * 1000000;

        while (n->completed < n->child_count) {
            int rc = pthread_cond_timedwait(&n->done_cond, &n->mu, &deadline);
            if (rc == ETIMEDOUT) {
                /* Cancel remaining children */
                for (int i = 0; i < n->child_count; i++) {
                    if (n->outcomes[i].exit_path == 2) {  /* still Z (running) */
                        atomic_store(&n->children[i]->cancel_requested, true);
                    }
                }
                n->crew_exit = 1;  /* N1: timeout is an error */
                break;
            }
        }
    } else {
        /* No timeout: wait forever (existing behavior) */
        while (n->completed < n->child_count) {
            pthread_cond_wait(&n->done_cond, &n->mu);
        }
    }

    int8_t result = n->crew_exit;
    pthread_mutex_unlock(&n->mu);
    return result;
}
```

Timeout fires cooperative cancellation (Issue 2), which gives wakes a chance to clean up via errdefer. If a wake is truly stuck and never checks `Cancelled()`, the crew eventually reports N1 (timeout) to its parent scope. The stuck wake becomes an orphan that the scheduler can eventually reap.

Default: no timeout (block forever). This preserves the simple case. Timeout is opt-in for cases where wakes might hang.

---

## Summary

| Issue | Severity | Fix complexity |
|-------|----------|---------------|
| 1. min() too aggressive | Medium | Low — add policy enum, default stays the same |
| 2. Cancel races | High | Medium — cooperative cancellation flag + Cancelled() builtin |
| 3. Borrow aliasing in crew | High | Medium — flow ownership check at crew scope |
| 4. Stale closure on restart | Medium | Low — define semantics, prefer parameters over captures |
| 5. Z overloaded | Low | None — document it, crew guarantees Z never escapes |
| 6. O(N) scheduling scan | Medium | Medium — per-crew run queue |
| 7. No timeout | Medium | Low — optional timeout with cooperative cancel |

Issues 2 and 3 are the most important. Cancel-on-error must be cooperative to avoid corrupting shared state. Borrow aliasing across wake boundaries must be caught by flow ownership. Everything else is refinement.

The architecture is correct. The pentit wake outcome, the crew as structured scope, the supervisor as handle/restart — all sound. These issues are the kind that surface during implementation, which is what the Furling Gate catches.
