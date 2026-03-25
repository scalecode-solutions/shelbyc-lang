/*
 * ShelbyC Runtime — Green Thread Scheduler
 *
 * M:N scheduler based on Go's GMP model. See runtime_sched.h for
 * the full design rationale and struct definitions.
 *
 * Build order within this file:
 *   1. Global state and TLS
 *   2. Stack management (mmap + guard page)
 *   3. Goroutine lifecycle (alloc, init, free, exit)
 *   4. Run queue operations (local + global, lock-free)
 *   5. Scheduler core (schedule, findrunnable, work stealing)
 *   6. Park/ready (wake blocking/waking)
 *   7. M (OS thread) management
 *   8. P (processor) management
 *   9. Public API (__sc_go, __sc_sched_init, __sc_sched_start)
 *  10. Yield / preemption
 */

#include "runtime_sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

/* Forward declarations for cross-references between sections */
static void wakep(void);
static void schedule(void);
static ScW *findrunnable(void);
static void execute(ScW *gp);
static void *m_thread_entry(void *arg);

/* ================================================================
 * 1. Global state and TLS
 * ================================================================ */

ScSched sc_sched;
__thread ScM *sc_m = NULL;  /* Current M for this OS thread */

static inline ScW *sc_curw(void) {
    return sc_m ? sc_m->curw : NULL;
}

static inline ScP *getp(void) {
    return sc_m ? sc_m->p : NULL;
}

ScW *sc_getw(void) { return sc_curw(); }
ScP *sc_getp(void) { return getp(); }

/* Monotonic timestamp in nanoseconds */
static uint64_t nanotime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ================================================================
 * 2. Stack management
 * ================================================================ */

void *sc_stack_alloc(void) {
    /* Allocate stack + guard page:
     *   [guard page (4KB, PROT_NONE)] [usable stack (64KB)]
     * Total allocation: SC_STACK_GUARD_SIZE + SC_STACK_SIZE
     *
     * Returns pointer to the START of the guard page.
     * Usable stack starts at ret + SC_STACK_GUARD_SIZE.
     * Stack grows downward: stack_hi is at ret + total_size.
     */
    size_t total = SC_STACK_GUARD_SIZE + SC_STACK_SIZE;
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "sc_stack_alloc: mmap failed\n");
        abort();
    }

    /* Protect the guard page at the bottom (lowest address) */
    if (mprotect(mem, SC_STACK_GUARD_SIZE, PROT_NONE) != 0) {
        fprintf(stderr, "sc_stack_alloc: mprotect failed\n");
        abort();
    }

    return mem;
}

void sc_stack_free(void *stack_mem) {
    size_t total = SC_STACK_GUARD_SIZE + SC_STACK_SIZE;
    munmap(stack_mem, total);
}

/* ================================================================
 * 3. Goroutine lifecycle
 * ================================================================ */

/* Allocate and initialize a new G. Does NOT put it on any run queue. */
static ScW *w_alloc(void) {
    ScW *gp = calloc(1, sizeof(ScW));
    if (!gp) { fprintf(stderr, "w_alloc: out of memory\n"); abort(); }

    /* Allocate stack */
    void *stack_mem = sc_stack_alloc();
    gp->stack_lo = (char *)stack_mem + SC_STACK_GUARD_SIZE;
    gp->stack_hi = (char *)gp->stack_lo + SC_STACK_SIZE;

    return gp;
}

/* Try to reuse a dead G from the P's free list. Returns NULL if none. */
static ScW *w_get_free(ScP *p) {
    ScW *gp = p->wfree;
    if (gp) {
        p->wfree = gp->schedlink;
        p->wfree_count--;
        memset(&gp->sched, 0, sizeof(ScContext));
        gp->schedlink = NULL;
        gp->param = NULL;
        gp->waiting = NULL;
        atomic_store_explicit(&gp->preempt, false, memory_order_relaxed);
    }
    return gp;
}

/* Put a dead G on the P's free list for reuse. */
static void w_put_free(ScP *p, ScW *gp) {
    gp->schedlink = p->wfree;
    p->wfree = gp;
    p->wfree_count++;
}

/* Assign a wake ID. Batch from global counter for locality. */
static uint64_t w_next_id(ScP *p) {
    if (p->widcache >= p->widcacheend) {
        /* Refill from global */
        p->widcache = atomic_fetch_add_explicit(
            &sc_sched.widgen, SC_WID_BATCH, memory_order_relaxed);
        p->widcacheend = p->widcache + SC_WID_BATCH;
    }
    uint64_t id = p->widcache;
    p->widcache++;
    return id;
}

/* Initialize context for a new wake so that when switched to,
 * it enters via __sc_context_entry which calls fn(arg). */
void sc_context_init(ScContext *ctx, void *stack_hi,
                     void (*entry)(void *), void *arg) {
    memset(ctx, 0, sizeof(ScContext));

    /* SP = top of stack, 16-byte aligned */
    ctx->sp = (uint64_t)stack_hi - 16;
    ctx->fp = 0;

    /* LR = trampoline that reads x19/x20 and calls fn(arg) */
    extern void __sc_context_entry(void);
    ctx->lr = (uint64_t)__sc_context_entry;

    /* x19 = fn, x20 = arg (callee-saved, survive the context switch) */
    ctx->x[0] = (uint64_t)entry;
    ctx->x[1] = (uint64_t)arg;
}

/* Called by __sc_context_entry when a wake's fn() returns.
 * Sets switch_reason and switches to w0. ALL cleanup (status change,
 * disconnect, free list) happens in execute() on w0's stack.
 * The wake's status remains W_RUNNING until execute() changes it —
 * this prevents other M's from touching the wake while we're on its stack. */
void __sc_wake_exit(void) {
    ScM *mp = sc_m;
    ScW *gp = mp->curw;

    /* If the main wake exits, terminate the process. */
    if (gp->wid == sc_sched.main_wid) {
        exit(sc_sched.main_exit_code);
    }

    /* Tell execute() what to do, then switch. Status stays W_RUNNING. */
    gp->switch_reason = SWITCH_EXIT;

    ScW *w0 = mp->w0;
    __sc_context_switch(&gp->sched, &w0->sched);

    /* Should never reach here */
    __builtin_unreachable();
}

/* ================================================================
 * 4. Run queue operations
 * ================================================================ */

/* Put g on the global run queue. Caller must hold sc_sched.lock. */
void sc_globrunqput(ScW *gp) {
    gp->schedlink = NULL;
    if (sc_sched.runq_tail) {
        sc_sched.runq_tail->schedlink = gp;
    } else {
        sc_sched.runq_head = gp;
    }
    sc_sched.runq_tail = gp;
    sc_sched.runq_size++;
}

/* Get a G from the global queue. Caller must hold sc_sched.lock.
 * Also grabs up to 'max' extra G's and puts them on p's local queue. */
ScW *sc_globrunqget(ScP *p, int32_t max) {
    if (sc_sched.runq_size == 0) return NULL;

    /* Take one for the caller */
    ScW *gp = sc_sched.runq_head;
    sc_sched.runq_head = gp->schedlink;
    if (!sc_sched.runq_head) sc_sched.runq_tail = NULL;
    sc_sched.runq_size--;
    gp->schedlink = NULL;

    /* Grab up to 'max' more for the local queue */
    int32_t n = 0;
    while (n < max && sc_sched.runq_size > 0) {
        ScW *extra = sc_sched.runq_head;
        sc_sched.runq_head = extra->schedlink;
        if (!sc_sched.runq_head) sc_sched.runq_tail = NULL;
        sc_sched.runq_size--;
        extra->schedlink = NULL;
        sc_runqput(p, extra, false);
        n++;
    }

    return gp;
}

/* Push g onto p's local run queue.
 * If next=true, try the runnext fast path.
 * If the local queue is full, put half onto the global queue. */
void sc_runqput(ScP *p, ScW *gp, bool next) {
    if (next) {
        /* Try to CAS into runnext */
        uintptr_t old = atomic_load_explicit(&p->runnext, memory_order_relaxed);
        if (atomic_compare_exchange_strong_explicit(
                &p->runnext, &old, (uintptr_t)gp,
                memory_order_release, memory_order_relaxed)) {
            if (old == 0) return;
            /* Had something in runnext — push that to the main queue */
            gp = (ScW *)old;
        }
    }

    /* Push to circular buffer */
    uint32_t h = atomic_load_explicit(&p->runqhead, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p->runqtail, memory_order_relaxed);

    if (t - h < SC_RUNQ_SIZE) {
        p->runq[t % SC_RUNQ_SIZE] = gp;
        atomic_store_explicit(&p->runqtail, t + 1, memory_order_release);
        return;
    }

    /* Local queue full — put half onto the global queue */
    pthread_mutex_lock(&sc_sched.lock);
    /* Move half of the local queue to global */
    uint32_t n = (t - h) / 2;
    for (uint32_t i = 0; i < n; i++) {
        ScW *stolen = p->runq[(h + i) % SC_RUNQ_SIZE];
        sc_globrunqput(stolen);
    }
    atomic_store_explicit(&p->runqhead, h + n, memory_order_release);
    /* Now there's space — add our new G */
    t = atomic_load_explicit(&p->runqtail, memory_order_relaxed);
    p->runq[t % SC_RUNQ_SIZE] = gp;
    atomic_store_explicit(&p->runqtail, t + 1, memory_order_release);
    pthread_mutex_unlock(&sc_sched.lock);
}

/* Pop a G from p's local run queue. Returns NULL if empty. */
ScW *sc_runqget(ScP *p) {
    /* Check runnext first */
    uintptr_t next = atomic_load_explicit(&p->runnext, memory_order_relaxed);
    if (next != 0) {
        if (atomic_compare_exchange_strong_explicit(
                &p->runnext, &next, 0,
                memory_order_acquire, memory_order_relaxed)) {
            return (ScW *)next;
        }
    }

    /* Pop from circular buffer */
    for (;;) {
        uint32_t h = atomic_load_explicit(&p->runqhead, memory_order_acquire);
        uint32_t t = atomic_load_explicit(&p->runqtail, memory_order_relaxed);
        if (t == h) return NULL;  /* Empty */

        ScW *gp = p->runq[h % SC_RUNQ_SIZE];
        if (atomic_compare_exchange_weak_explicit(
                &p->runqhead, &h, h + 1,
                memory_order_release, memory_order_relaxed)) {
            return gp;
        }
        /* CAS failed (stealer raced) — retry */
    }
}

/* Steal up to half of p2's run queue into a local batch, then CAS.
 * Returns one W to run immediately. Follows Go's runqgrab pattern:
 * copy items into local array FIRST, then atomically advance head.
 * If CAS fails, no harm done — we just retry or give up. */
ScW *sc_runqsteal(ScP *p, ScP *p2) {
    /* Try to steal runnext first */
    uintptr_t next = atomic_load_explicit(&p2->runnext, memory_order_acquire);
    if (next != 0) {
        if (atomic_compare_exchange_strong_explicit(
                &p2->runnext, &next, 0,
                memory_order_acquire, memory_order_relaxed)) {
            return (ScW *)next;
        }
    }

    /* Snapshot the victim's queue bounds (both load-acquire per Go) */
    uint32_t h = atomic_load_explicit(&p2->runqhead, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p2->runqtail, memory_order_acquire);
    int32_t n = (int32_t)(t - h);
    if (n <= 0) return NULL;

    n = n - n / 2;  /* Steal half (Go rounds down for the stealer) */
    if (n <= 0) return NULL;
    if (n > SC_RUNQ_SIZE / 2) n = SC_RUNQ_SIZE / 2;

    /* Copy items into a local batch BEFORE the CAS.
     * If the CAS fails, these copies are harmless. */
    ScW *batch[SC_RUNQ_SIZE / 2];
    for (int32_t i = 0; i < n; i++) {
        batch[i] = p2->runq[(h + (uint32_t)i) % SC_RUNQ_SIZE];
    }

    /* Atomically advance head. If this fails, another stealer or the
     * owner already consumed these items. Give up — no harm done. */
    if (!atomic_compare_exchange_strong_explicit(
            &p2->runqhead, &h, h + (uint32_t)n,
            memory_order_release, memory_order_relaxed)) {
        return NULL;
    }

    /* CAS succeeded — we own these items now. Return one, enqueue rest. */
    ScW *gp = batch[n - 1];
    for (int32_t i = 0; i < n - 1; i++) {
        sc_runqput(p, batch[i], false);
    }

    return gp;
}

/* ================================================================
 * 5. Scheduler core
 * ================================================================ */

/* The main scheduler loop. Runs on w0's stack.
 * Finds a runnable G and switches to it. Loops forever. */
static void schedule(void) {
    for (;;) {
        ScM *mp = sc_m;
        ScP *pp = mp->p;

        if (!pp) {
            fprintf(stderr, "schedule: M has no P\n");
            abort();
        }

        pp->schedtick++;

        ScW *gp = findrunnable();  /* May block */

        /* Execute the wake — returns when G parks or exits */
        execute(gp);

        /* Back on w0's stack. Loop to find next work. */
    }
}

/* Find a runnable wake. Checks in priority order:
 * 1. Local run queue
 * 2. Global run queue (every 61 ticks for fairness)
 * 3. Work stealing from other P's
 * 4. Global run queue (final check)
 * 5. Sleep until work arrives */
static ScW *findrunnable(void) {
    ScP *pp = sc_m->p;
    ScW *gp;

    /* Every 61 ticks, check global queue first for fairness */
    if (pp->schedtick % 61 == 0) {
        pthread_mutex_lock(&sc_sched.lock);
        gp = sc_globrunqget(pp, 1);
        pthread_mutex_unlock(&sc_sched.lock);
        if (gp) return gp;
    }

    /* 1. Local run queue */
    gp = sc_runqget(pp);
    if (gp) return gp;

    /* 2. Global run queue */
    pthread_mutex_lock(&sc_sched.lock);
    gp = sc_globrunqget(pp, pp->id == 0 ? 1 : 0);
    pthread_mutex_unlock(&sc_sched.lock);
    if (gp) return gp;

    /* 3. Work stealing */
    if (sc_sched.maxprocs > 1) {
        /* Try 4 rounds of stealing from random peers */
        for (int round = 0; round < 4; round++) {
            for (int32_t i = 0; i < sc_sched.maxprocs; i++) {
                ScP *p2 = sc_sched.allp[(pp->id + i + 1) % sc_sched.maxprocs];
                if (p2 == pp) continue;
                if (p2->status != P_RUNNING) continue;

                gp = sc_runqsteal(pp, p2);
                if (gp) return gp;
            }
        }
    }

    /* 4. Final global queue check */
    pthread_mutex_lock(&sc_sched.lock);
    gp = sc_globrunqget(pp, 0);
    pthread_mutex_unlock(&sc_sched.lock);
    if (gp) return gp;

    /* 5. No work anywhere. Spin briefly, then sleep. */
    for (int spin = 0; spin < 100; spin++) {
        /* Check local queue (maybe someone pushed while we were stealing) */
        gp = sc_runqget(pp);
        if (gp) return gp;

        /* Brief pause to reduce bus contention */
        __asm__ volatile("yield" ::: "memory");
    }

    /* Still nothing — park this M until wakep() signals us.
     * Release the P first so another M can use it. */
    ScM *mp = sc_m;

    pthread_mutex_lock(&sc_sched.lock);

    /* Release P to idle list */
    pp->m = NULL;
    pp->status = P_IDLE;
    pp->link = sc_sched.pidle;
    sc_sched.pidle = pp;
    atomic_fetch_add_explicit(&sc_sched.npidle, 1, memory_order_release);
    mp->p = NULL;

    /* Check global queue one more time before sleeping
     * (someone may have added work between our check and locking) */
    if (sc_sched.runq_size > 0) {
        /* Reclaim our P */
        sc_sched.pidle = pp->link;
        atomic_fetch_sub_explicit(&sc_sched.npidle, 1, memory_order_release);
        pp->link = NULL;
        pp->status = P_RUNNING;
        pp->m = mp;
        mp->p = pp;
        gp = sc_globrunqget(pp, 0);
        pthread_mutex_unlock(&sc_sched.lock);
        if (gp) return gp;
    } else {
        /* Put M on idle list and sleep */
        mp->schedlink = sc_sched.midle;
        sc_sched.midle = mp;
        sc_sched.nmidle++;
        pthread_mutex_unlock(&sc_sched.lock);

        /* Sleep until woken by wakep() */
        pthread_mutex_lock(&mp->park_mu);
        while (!mp->park_note) {
            pthread_cond_wait(&mp->park_cond, &mp->park_mu);
        }
        mp->park_note = false;
        pthread_mutex_unlock(&mp->park_mu);

        /* Woken up — we should have a P assigned by wakep() */
        if (!mp->p) {
            /* Try to acquire an idle P */
            pthread_mutex_lock(&sc_sched.lock);
            ScP *newp = sc_sched.pidle;
            if (newp) {
                sc_sched.pidle = newp->link;
                atomic_fetch_sub_explicit(&sc_sched.npidle, 1, memory_order_release);
                newp->link = NULL;
                newp->status = P_RUNNING;
                newp->m = mp;
                mp->p = newp;
            }
            pthread_mutex_unlock(&sc_sched.lock);

            if (!mp->p) {
                /* No P available — re-park */
                return findrunnable();
            }
        }

        /* Retry finding work */
        return findrunnable();
    }

    /* Unreachable */
    abort();
}

/* Switch to wake gp. Sets it as M's curw and context-switches.
 * Returns when the wake switches back to w0 (park, yield, or exit).
 *
 * ALL post-switch cleanup happens here on w0's stack. This is the
 * mcall pattern from Go: the wake's status remains W_RUNNING until
 * this function changes it. No other M can touch the wake while its
 * status is W_RUNNING. This eliminates the race where another M
 * steals the wake from a queue while we're still on its stack. */
static void execute(ScW *gp) {
    ScM *mp = sc_m;

    /* Bind W to M. Status must already be W_RUNNABLE from the run queue. */
    gp->m = mp;
    mp->curw = gp;
    gp->switch_reason = SWITCH_NONE;
    atomic_store_explicit(&gp->status, W_RUNNING, memory_order_release);
    gp->preempt_tick = nanotime();

    /* Switch from w0 to gp */
    __sc_context_switch(&mp->w0->sched, &gp->sched);

    /* ================================================================
     * Back on w0's stack. gp's status is STILL W_RUNNING.
     * No other M can touch gp until we change its status.
     * This is the critical safety window — do all cleanup here.
     * ================================================================ */

    ScSwitchReason reason = gp->switch_reason;

    /* Disconnect W from M (dropg equivalent) */
    gp->m = NULL;
    mp->curw = NULL;

    switch (reason) {
    case SWITCH_EXIT:
        /* Wake finished. Mark dead, put on free list. */
        atomic_store_explicit(&gp->status, W_DEAD, memory_order_release);
        if (mp->p) {
            w_put_free(mp->p, gp);
        }
        break;

    case SWITCH_YIELD:
        /* Wake yielded. Mark runnable, put back on run queue. */
        atomic_store_explicit(&gp->status, W_RUNNABLE, memory_order_release);
        if (mp->p) {
            sc_runqput(mp->p, gp, false);
        } else {
            pthread_mutex_lock(&sc_sched.lock);
            sc_globrunqput(gp);
            pthread_mutex_unlock(&sc_sched.lock);
        }
        break;

    case SWITCH_PARK: {
        /* Wake wants to park. Mark waiting, then call unlock function.
         * The status change to W_WAITING MUST happen before the unlock,
         * because the unlock may allow another thread to call sc_ready()
         * which needs to see W_WAITING to transition to W_RUNNABLE. */
        atomic_store_explicit(&gp->status, W_WAITING, memory_order_release);

        if (gp->park_fn) {
            bool ok = gp->park_fn(gp->park_lock);
            if (!ok) {
                /* Unlock said don't park — make runnable again.
                 * This is rare (used for aborted select). */
                atomic_store_explicit(&gp->status, W_RUNNABLE, memory_order_release);
                if (mp->p) {
                    sc_runqput(mp->p, gp, true);
                } else {
                    pthread_mutex_lock(&sc_sched.lock);
                    sc_globrunqput(gp);
                    pthread_mutex_unlock(&sc_sched.lock);
                }
            }
            gp->park_fn = NULL;
            gp->park_lock = NULL;
        }
        break;
    }

    default:
        fprintf(stderr, "execute: unknown switch_reason %d\n", reason);
        abort();
    }
}

/* ================================================================
 * 6. Park / Ready
 * ================================================================ */

/* Park the current wake. The wake remains W_RUNNING until execute()
 * changes it to W_WAITING on w0's stack. This is the mcall pattern:
 * status is the lock — while W_RUNNING, no other M can touch this wake.
 *
 * unlock_fn is called by execute() on w0's stack AFTER the status
 * changes to W_WAITING. This prevents lost wakeups: sc_ready() will
 * see W_WAITING and correctly transition to W_RUNNABLE. */
void sc_park(bool (*unlock_fn)(void *), void *lock) {
    ScM *mp = sc_m;
    ScW *gp = mp->curw;

    /* Store the park request. execute() will process it on w0's stack. */
    gp->switch_reason = SWITCH_PARK;
    gp->park_fn = unlock_fn;
    gp->park_lock = lock;

    /* Switch to w0. Status stays W_RUNNING until execute() changes it. */
    __sc_context_switch(&gp->sched, &mp->w0->sched);

    /* Resumed — execute() set us back to W_RUNNING before switching here. */
}

/* Wake a parked wake. Puts it on the current P's run queue. */
void sc_ready(ScW *gp) {
    uint32_t expected = W_WAITING;
    if (!atomic_compare_exchange_strong_explicit(
            &gp->status, &expected, W_RUNNABLE,
            memory_order_release, memory_order_relaxed)) {
        /* Already not waiting — double wakeup or race. Ignore. */
        return;
    }

    /* Put on the current P's run queue if we have one */
    ScP *pp = getp();
    if (pp) {
        sc_runqput(pp, gp, true);  /* next=true for cache locality */
    } else {
        /* No P — put on global queue */
        pthread_mutex_lock(&sc_sched.lock);
        sc_globrunqput(gp);
        pthread_mutex_unlock(&sc_sched.lock);
    }

    /* Wake an idle M if there is one */
    if (atomic_load_explicit(&sc_sched.npidle, memory_order_acquire) > 0) {
        wakep();
    }
}

/* ================================================================
 * 7. M (OS thread) management
 * ================================================================ */

/* Allocate and initialize a new M. Does NOT start the OS thread. */
static ScM *m_alloc(void) {
    ScM *mp = calloc(1, sizeof(ScM));
    if (!mp) { fprintf(stderr, "m_alloc: out of memory\n"); abort(); }

    /* Allocate w0 (scheduler wake) — uses a system stack */
    mp->w0 = calloc(1, sizeof(ScW));
    if (!mp->w0) { fprintf(stderr, "m_alloc: w0 out of memory\n"); abort(); }

    /* w0 gets a real stack for the scheduler to run on */
    void *stack_mem = sc_stack_alloc();
    mp->w0->stack_lo = (char *)stack_mem + SC_STACK_GUARD_SIZE;
    mp->w0->stack_hi = (char *)mp->w0->stack_lo + SC_STACK_SIZE;
    mp->w0->wid = 0;  /* w0 has no wake ID */

    pthread_mutex_init(&mp->park_mu, NULL);
    pthread_cond_init(&mp->park_cond, NULL);
    mp->park_note = false;

    mp->id = atomic_fetch_add_explicit(&sc_sched.widgen, 1,
                                        memory_order_relaxed);
    return mp;
}

/* Start an M's OS thread. */
static void m_start(ScM *mp) {
    int err = pthread_create(&mp->thread, NULL, m_thread_entry, mp);
    if (err != 0) {
        fprintf(stderr, "m_start: pthread_create failed: %d\n", err);
        abort();
    }
    pthread_detach(mp->thread);
}

/* Entry point for M worker threads. */
static void *m_thread_entry(void *arg) {
    ScM *mp = (ScM *)arg;
    sc_m = mp;  /* Set TLS */

    /* Set up w0's context so schedule() has a stack to return to.
     * We're already running on the OS thread's stack, so we save
     * the current context as w0's context. */

    /* The scheduler loop runs on w0's stack.
     * We use this OS thread's stack as w0's stack. */
    schedule();

    /* schedule() never returns */
    return NULL;
}

/* Wake an idle M to run wakes, or start a new one.
 * Following Go's protocol: only wake if no spinning M exists. */
static void wakep(void) {
    /* If there's already a spinning M looking for work, don't wake another.
     * The spinning M will find the new work. */
    if (atomic_load_explicit(&sc_sched.nmspinning, memory_order_acquire) > 0) {
        return;
    }

    /* Don't wake if there are no idle P's */
    if (atomic_load_explicit(&sc_sched.npidle, memory_order_acquire) <= 0) {
        return;
    }

    pthread_mutex_lock(&sc_sched.lock);

    /* Grab an idle P */
    ScP *pp = sc_sched.pidle;
    if (!pp) {
        pthread_mutex_unlock(&sc_sched.lock);
        return;
    }
    sc_sched.pidle = pp->link;
    atomic_fetch_sub_explicit(&sc_sched.npidle, 1, memory_order_release);
    pp->link = NULL;
    pp->status = P_RUNNING;

    /* Try to wake an idle M */
    ScM *mp = sc_sched.midle;
    if (mp) {
        sc_sched.midle = mp->schedlink;
        sc_sched.nmidle--;
        mp->schedlink = NULL;

        /* Assign P to M */
        pp->m = mp;
        mp->p = pp;

        pthread_mutex_unlock(&sc_sched.lock);

        /* Wake the M */
        pthread_mutex_lock(&mp->park_mu);
        mp->park_note = true;
        pthread_cond_signal(&mp->park_cond);
        pthread_mutex_unlock(&mp->park_mu);
        return;
    }

    /* No idle M — need a new one */
    sc_sched.mcount++;
    pthread_mutex_unlock(&sc_sched.lock);

    ScM *newm = m_alloc();
    pp->m = newm;
    newm->p = pp;
    m_start(newm);
}

/* ================================================================
 * 8. P (processor) management
 * ================================================================ */

static ScP *p_alloc(int32_t id) {
    ScP *pp = calloc(1, sizeof(ScP));
    if (!pp) { fprintf(stderr, "p_alloc: out of memory\n"); abort(); }
    pp->id = id;
    pp->status = P_IDLE;
    return pp;
}

/* ================================================================
 * 9. Public API
 * ================================================================ */

/* Initialize the scheduler. Called once before anything else. */
void __sc_sched_init(int nprocs) {
    if (nprocs <= 0) {
        nprocs = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (nprocs <= 0) nprocs = 1;
    }
    if (nprocs > SC_MAXPROCS_MAX) nprocs = SC_MAXPROCS_MAX;

    memset(&sc_sched, 0, sizeof(ScSched));
    pthread_mutex_init(&sc_sched.lock, NULL);
    sc_sched.maxprocs = nprocs;
    sc_sched.maxmcount = 10000;  /* Sanity limit on OS threads */
    atomic_store(&sc_sched.widgen, 1);  /* Start IDs at 1 */

    /* Allocate P's */
    sc_sched.allp = calloc(nprocs, sizeof(ScP *));
    for (int i = 0; i < nprocs; i++) {
        sc_sched.allp[i] = p_alloc(i);
    }

    /* P[0] will be used by the main M. Rest go on idle list. */
    for (int i = nprocs - 1; i >= 1; i--) {
        sc_sched.allp[i]->link = sc_sched.pidle;
        sc_sched.pidle = sc_sched.allp[i];
        atomic_fetch_add_explicit(&sc_sched.npidle, 1, memory_order_relaxed);
    }
}

/* Start the scheduler and run Main(). */
void __sc_sched_start(void (*main_fn)(void)) {
    /* Set up the main M (M0) — this OS thread becomes M0 */
    ScM *m0 = m_alloc();
    sc_m = m0;  /* TLS */

    /* Attach P[0] to M0 */
    ScP *p0 = sc_sched.allp[0];
    p0->status = P_RUNNING;
    p0->m = m0;
    m0->p = p0;
    sc_sched.mcount = 1;

    atomic_store(&sc_sched.mainstarted, true);

    /* Create a G for Main() and put it on the run queue */
    ScW *main_g = w_get_free(p0);
    if (!main_g) main_g = w_alloc();

    main_g->wid = w_next_id(p0);
    sc_sched.main_wid = main_g->wid;
    main_g->fn = (void (*)(void *))main_fn;
    main_g->fn_arg = NULL;
    atomic_store_explicit(&main_g->status, W_RUNNABLE, memory_order_release);

    sc_context_init(&main_g->sched, main_g->stack_hi,
                    (void (*)(void *))main_fn, NULL);

    sc_runqput(p0, main_g, true);

    /* Set up w0's context: save current (OS thread) stack as w0.
     * When wakes switch back to w0, they resume here —
     * inside schedule(). */

    /* w0 uses the current OS thread stack, not a separate allocation.
     * We set up w0->sched so that when execute() does
     * __sc_context_switch(&w0->sched, &gp->sched), the return
     * from that switch lands back in execute() on this OS thread's stack. */

    /* Enter the scheduler loop — never returns */
    schedule();
}

/* Spawn a new wake: go fn(arg) */
void __sc_wake_spawn(void (*fn)(void *), void *arg) {
    ScP *pp = getp();

    /* Get or allocate a G */
    ScW *gp = NULL;
    if (pp) gp = w_get_free(pp);
    if (!gp) gp = w_alloc();

    /* Initialize */
    gp->wid = pp ? w_next_id(pp) : atomic_fetch_add(&sc_sched.widgen, 1);
    gp->fn = fn;
    gp->fn_arg = arg;
    atomic_store_explicit(&gp->status, W_RUNNABLE, memory_order_release);

    sc_context_init(&gp->sched, gp->stack_hi, fn, arg);

    /* Put on run queue */
    if (pp) {
        sc_runqput(pp, gp, true);
    } else {
        pthread_mutex_lock(&sc_sched.lock);
        sc_globrunqput(gp);
        pthread_mutex_unlock(&sc_sched.lock);
    }

    /* Wake an idle M if there are idle P's */
    if (atomic_load_explicit(&sc_sched.npidle, memory_order_acquire) > 0 &&
        atomic_load_explicit(&sc_sched.mainstarted, memory_order_acquire)) {
        wakep();
    }
}

/* ================================================================
 * 10. Yield / preemption
 * ================================================================ */

/* Called at function prologues and loop back-edges.
 * Yields to the scheduler to allow other wakes to run.
 * Status stays W_RUNNING until execute() changes it on w0's stack. */
void __sc_wake_yield(void) {
    ScW *gp = sc_curw();
    if (!gp) return;

    ScM *mp = sc_m;
    ScP *pp = mp->p;
    if (!pp) return;

    /* Only yield if there's other work to do, or preemption was requested. */
    bool has_work = (atomic_load_explicit(&pp->runnext, memory_order_relaxed) != 0) ||
                    (atomic_load_explicit(&pp->runqhead, memory_order_relaxed) !=
                     atomic_load_explicit(&pp->runqtail, memory_order_relaxed));
    bool preempted = atomic_load_explicit(&gp->preempt, memory_order_relaxed);

    if (!has_work && !preempted) return;

    if (preempted) {
        atomic_store_explicit(&gp->preempt, false, memory_order_relaxed);
    }

    /* Tell execute() to re-enqueue us, then switch. Status stays W_RUNNING. */
    gp->switch_reason = SWITCH_YIELD;

    __sc_context_switch(&gp->sched, &mp->w0->sched);

    /* Resumed — execute() set us back to W_RUNNING before switching here. */
}

/* ================================================================
 * Sudog pool (simple for now — allocate/free)
 * ================================================================ */

ScSudog *sc_sudog_acquire(void) {
    ScSudog *s = calloc(1, sizeof(ScSudog));
    return s;
}

void sc_sudog_release(ScSudog *s) {
    free(s);
}

/* ================================================================
 * Wait queue operations
 * ================================================================ */

void sc_waitq_enqueue(ScWaitQ *q, ScSudog *s) {
    s->next = NULL;
    s->prev = q->last;
    if (q->last) {
        q->last->next = s;
    } else {
        q->first = s;
    }
    q->last = s;
}

ScSudog *sc_waitq_dequeue(ScWaitQ *q) {
    ScSudog *s = q->first;
    if (!s) return NULL;
    q->first = s->next;
    if (q->first) {
        q->first->prev = NULL;
    } else {
        q->last = NULL;
    }
    s->next = NULL;
    s->prev = NULL;
    return s;
}
