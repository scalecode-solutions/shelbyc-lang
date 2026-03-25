/*
 * ShelbyC Runtime — Green Thread Scheduler
 *
 * M:N scheduler based on Go's GMP model:
 *   W (wake)       — unit of work, user-level green thread
 *   M (machine)    — OS thread, runs wakes
 *   P (processor)  — logical CPU, owns a local run queue
 *
 * Key invariants:
 *   - At most MAXPROCS wakes run simultaneously
 *   - Each M has at most one P attached
 *   - Each P has at most one M attached
 *   - Work stealing keeps all P's busy
 *   - Blocking a G parks it (yields the M), not blocks the OS thread
 *
 * Context switch: raw arm64 assembly (see runtime_asm_arm64.s)
 * Stack model: fixed 64KB per wake with mmap'd guard page
 * Preemption: cooperative (yield points at fn entry + loop back-edges)
 *
 * See Go's src/runtime/runtime2.go and src/runtime/proc.go for the
 * full model. This is a simplified version for the C bootstrap.
 */

#ifndef SC_RUNTIME_SCHED_H
#define SC_RUNTIME_SCHED_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* ---- Constants ---- */

#define SC_STACK_SIZE       (8 * 1024 * 1024)  /* 8MB per wake stack */
#define SC_STACK_GUARD_SIZE (4096)         /* Guard page at bottom (PROT_NONE) */
#define SC_RUNQ_SIZE        256            /* Per-P local run queue capacity */
#define SC_MAXPROCS_MAX   256            /* Max logical processors */
#define SC_WID_BATCH       16             /* Goroutine ID batch per P */
#define SC_PREEMPT_TICK_MS  10             /* Preemption check interval (ms) */

/* ---- Forward declarations ---- */

typedef struct ScW     ScW;
typedef struct ScM     ScM;
typedef struct ScP     ScP;
typedef struct ScSched ScSched;
typedef struct ScSudog ScSudog;
typedef struct ScWaitQ ScWaitQ;

/* ---- Goroutine status ---- */

typedef enum {
    W_IDLE      = 0,  /* Just allocated, not yet initialized */
    W_RUNNABLE  = 1,  /* On a run queue, ready to execute */
    W_RUNNING   = 2,  /* Executing on an M, owns its stack */
    W_WAITING   = 4,  /* Blocked (on channel, lock, etc.) */
    W_DEAD      = 6,  /* Finished, on free list for reuse */
} ScWStatus;

/* ---- Switch reason (set by wake before switching to w0) ---- */

typedef enum {
    SWITCH_NONE  = 0,
    SWITCH_PARK  = 1,  /* sc_park: block on channel/lock */
    SWITCH_YIELD = 2,  /* __sc_wake_yield: cooperative preemption */
    SWITCH_EXIT  = 3,  /* __sc_wake_exit: wake finished */
} ScSwitchReason;

/* ---- Processor status ---- */

typedef enum {
    P_IDLE    = 0,  /* Not running any code, available */
    P_RUNNING = 1,  /* Attached to an M, running user code */
    P_DEAD    = 4,  /* No longer used */
} ScPStatus;

/* ---- Saved wake context (arm64) ----
 *
 * On arm64 (AAPCS64), callee-saved registers:
 *   GP:   x19-x28, x29 (FP), x30 (LR)
 *   SIMD: d8-d15 (lower 64 bits of v8-v15)
 *   SP:   stack pointer
 *
 * We save all of these for correctness — any wake may use
 * floats, and the context switch must be transparent.
 *
 * Total: 13 GP + 8 FP = 21 registers = 168 bytes
 *
 * NOTE: x18 is reserved by macOS. Do NOT touch it.
 */

typedef struct {
    uint64_t sp;          /* Stack pointer */
    uint64_t lr;          /* Link register (x30) — resume address */
    uint64_t fp;          /* Frame pointer (x29) */
    uint64_t x[10];       /* x19-x28 */
    uint64_t d[8];        /* d8-d15 (callee-saved FP/SIMD) */
} ScContext;

/* ---- G (Goroutine) ---- */

struct ScW {
    /* Context for resumption */
    ScContext       sched;

    /* Stack bounds */
    void           *stack_lo;       /* Bottom of stack (lowest address) */
    void           *stack_hi;       /* Top of stack (highest address) */

    /* Scheduling state */
    _Atomic uint32_t status;        /* ScWStatus — atomic for cross-M visibility */
    uint64_t        wid;           /* Unique wake ID */
    ScW            *schedlink;      /* Next G in a linked list (run queue, free list) */
    ScM            *m;              /* Current M executing this G (NULL if not running) */

    /* Parking / wakeup */
    void           *param;          /* Generic wakeup parameter */
    ScSudog        *waiting;        /* Sudog if blocked on a channel */

    /* Switch request — set before switching to w0, read by execute() on w0.
     * This is the mcall pattern: the wake says what it wants, switches to w0,
     * and execute() does the cleanup on w0's stack where it's safe. */
    ScSwitchReason  switch_reason;  /* Why we're switching to w0 */
    bool          (*park_fn)(void *); /* Unlock function for SWITCH_PARK */
    void           *park_lock;      /* Lock argument for park_fn */

    /* Preemption */
    _Atomic bool    preempt;        /* Preemption requested */
    uint64_t        preempt_tick;   /* Timestamp when execution started (for sysmon) */

    /* Entry point (for initial launch) */
    void           (*fn)(void *);   /* Function to execute */
    void           *fn_arg;         /* Argument to fn */
};

/* ---- M (Machine / OS Thread) ---- */

struct ScM {
    ScW            *w0;             /* Scheduler wake (runs on system stack) */
    ScW            *curw;           /* Currently running user wake */
    ScP            *p;              /* Currently attached processor */
    int64_t         id;             /* Thread ID for debugging */

    /* Thread management */
    pthread_t       thread;         /* OS thread handle */
    bool            spinning;       /* Actively looking for work (no P attachment change) */
    bool            blocked;        /* Parked, waiting for a P */

    /* Parking mechanism (when no P available) */
    pthread_mutex_t park_mu;        /* Mutex for park/unpark */
    pthread_cond_t  park_cond;      /* Condvar for park/unpark */
    bool            park_note;      /* Wakeup flag */

    /* Linked list of idle M's */
    ScM            *schedlink;      /* Next in idle list */
};

/* ---- P (Processor / Run Queue) ---- */

struct ScP {
    int32_t         id;             /* Processor ID (0 to maxprocs-1) */
    uint32_t        status;         /* ScPStatus */
    ScM            *m;              /* Attached OS thread */

    /* Local run queue: lock-free circular buffer
     * Only the owning M writes (push/pop). Stealers read via atomics.
     * Modeled after Go's per-P runq. */
    _Atomic uint32_t runqhead;      /* Consumer index (owner pops, stealers steal) */
    _Atomic uint32_t runqtail;      /* Producer index (owner pushes) */
    ScW            *runq[SC_RUNQ_SIZE]; /* Circular buffer of runnable G's */

    /* Fast path: next G to run (avoids touching the ring buffer) */
    _Atomic uintptr_t runnext;      /* ScW* cast to uintptr_t for atomic CAS */

    /* Free G cache (dead wakes for reuse) */
    ScW            *wfree;          /* Free list head */
    int32_t         wfree_count;    /* Number of free G's cached */

    /* Goroutine ID cache (batch allocation from global) */
    uint64_t        widcache;      /* Next ID to assign */
    uint64_t        widcacheend;   /* End of current batch */

    /* Scheduling stats */
    uint32_t        schedtick;      /* Incremented on every schedule() call */

    /* Linked list of idle P's */
    ScP            *link;           /* Next in idle list */
};

/* ---- Sudog (Channel Wait Queue Element) ----
 *
 * When a wake blocks on a channel send/recv, a sudog is
 * enqueued on the channel's wait queue. When the counterpart
 * operation arrives, the sudog is dequeued and the wake
 * is woken via sc_ready().
 */

struct ScSudog {
    ScW            *g;              /* The waiting wake */
    ScSudog        *next;           /* Next in wait queue */
    ScSudog        *prev;           /* Prev in wait queue */
    void           *elem;           /* Pointer to send/recv data */
    bool            success;        /* Did the operation complete? (false = chan closed) */
    bool            is_select;      /* Part of a select statement? */
};

/* ---- Wait Queue (doubly-linked sudog list) ---- */

struct ScWaitQ {
    ScSudog        *first;
    ScSudog        *last;
};

/* ---- Global Scheduler State ---- */

struct ScSched {
    pthread_mutex_t lock;           /* Protects global queues and idle lists */

    /* Global run queue (overflow from full local queues) */
    ScW            *runq_head;      /* FIFO linked list via schedlink */
    ScW            *runq_tail;
    int32_t         runq_size;

    /* Processor pool */
    ScP           **allp;           /* Array of all P's [0..maxprocs) */
    int32_t         maxprocs;     /* Number of logical processors */

    /* Idle lists */
    ScP            *pidle;          /* Idle P linked list */
    _Atomic int32_t npidle;         /* Count of idle P's */
    ScM            *midle;          /* Idle M linked list */
    int32_t         nmidle;         /* Count of idle M's */

    /* Thread management */
    _Atomic int32_t nmspinning;     /* M's actively looking for work */
    int32_t         maxmcount;      /* Maximum number of M's */
    int32_t         mcount;         /* Current number of M's */

    /* Goroutine ID generator */
    _Atomic uint64_t widgen;       /* Global ID counter */

    /* Scheduler state */
    _Atomic bool    mainstarted;    /* Has Main() been called? */
    bool            stopped;        /* Shutdown requested */
    uint64_t        main_wid;       /* Wake ID of Main() — exit process when it dies */
    int             main_exit_code; /* Exit code from Main() (if i32 return) */
};

/* ---- Global state (single instance) ---- */

extern ScSched  sc_sched;          /* The global scheduler */
extern __thread ScM *sc_m;         /* Current M (thread-local) */

/* ---- Public API (called by generated code) ---- */

/* Initialize the scheduler. Called once before Main().
 * nprocs = number of logical processors (0 = use CPU count). */
void __sc_sched_init(int nprocs);

/* Start the scheduler and run Main(). Does not return until exit. */
void __sc_sched_start(void (*main_fn)(void));

/* Spawn a new wake. Called by ND_GO codegen.
 * fn(arg) will execute on a new green thread. */
void __sc_wake_spawn(void (*fn)(void *), void *arg);

/* Yield check. Called at function prologues and loop back-edges.
 * If preemption is requested, yields to the scheduler. */
void __sc_wake_yield(void);

/* ---- Internal API (used by runtime subsystems) ---- */

/* Park the current wake. Sets status to W_WAITING.
 * unlock_fn(lock) is called after the G is parked (prevents lost wakeups).
 * Returns when the wake is woken via sc_ready(). */
void sc_park(bool (*unlock_fn)(void *), void *lock);

/* Wake a parked wake. Sets status to W_RUNNABLE and
 * puts it on the current P's run queue. */
void sc_ready(ScW *gp);

/* Get the currently running wake. */
ScW *sc_getw(void);

/* Get the current P. */
ScP *sc_getp(void);

/* ---- Run queue operations ---- */

/* Push g onto p's local run queue. If next=true, put in runnext slot.
 * If the local queue is full, puts half onto the global queue. */
void sc_runqput(ScP *p, ScW *gp, bool next);

/* Pop a wake from p's local run queue. Returns NULL if empty. */
ScW *sc_runqget(ScP *p);

/* Steal up to half of p2's run queue into p's queue. Returns one G
 * to run immediately, or NULL if nothing to steal. */
ScW *sc_runqsteal(ScP *p, ScP *p2);

/* Put g on the global run queue. Caller must hold sc_sched.lock. */
void sc_globrunqput(ScW *gp);

/* Get a batch of wakes from the global queue. Returns one to run,
 * puts up to max others onto p's local queue. Caller holds sc_sched.lock. */
ScW *sc_globrunqget(ScP *p, int32_t max);

/* ---- Stack management ---- */

/* Allocate a wake stack (SC_STACK_SIZE + guard page).
 * Returns the stack_lo pointer. stack_hi = stack_lo + SC_STACK_SIZE. */
void *sc_stack_alloc(void);

/* Free a wake stack. */
void sc_stack_free(void *stack_lo);

/* ---- Sudog pool ---- */

ScSudog *sc_sudog_acquire(void);
void     sc_sudog_release(ScSudog *s);

/* ---- Wait queue operations ---- */

void     sc_waitq_enqueue(ScWaitQ *q, ScSudog *s);
ScSudog *sc_waitq_dequeue(ScWaitQ *q);

/* ---- Context switch (implemented in assembly) ---- */

/* Save current context into 'from', restore context from 'to'.
 * This is the core context switch — saves callee-saved registers
 * (x19-x28, x29/FP, x30/LR, d8-d15, SP) and jumps to the
 * saved PC in 'to'. */
void __sc_context_switch(ScContext *from, ScContext *to);

/* Initialize a context for a new wake. Sets up the stack
 * so that when switched to, it calls entry(arg) and then returns
 * to the scheduler. Implemented in C, uses the assembly switch. */
void sc_context_init(ScContext *ctx, void *stack_hi,
                     void (*entry)(void *), void *arg);

#endif /* SC_RUNTIME_SCHED_H */
