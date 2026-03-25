/*
 * ShelbyC Runtime — Channel Implementation
 *
 * Channels use the green thread scheduler for blocking:
 *   - Send to full channel: park sender wake, wake when space available
 *   - Recv from empty channel: park receiver wake, wake when data available
 *   - Direct copy: if a receiver is waiting when sender arrives (or vice versa),
 *     copy data directly between wake stacks — no buffer touch
 *
 * Based on Go's chan.go. Key differences from the old pthread version:
 *   - No pthread_cond_wait — uses sc_park/sc_ready (green thread park/wake)
 *   - Unbuffered channels now work (were "not yet implemented")
 *   - Wait queues (sudog linked lists) instead of condition variables
 *   - Send to closed channel panics (was silently ignored)
 *
 * Lock ordering: channel lock is held during enqueue/dequeue of sudogs.
 * sc_park atomically releases the lock after parking the wake.
 */

#include "runtime_sched.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Channel structure ---- */

typedef struct {
    /* Buffer (ring buffer for buffered channels, NULL for unbuffered) */
    void           *buf;
    size_t          elem_size;
    size_t          dataqsiz;       /* Buffer capacity (0 = unbuffered) */
    size_t          qcount;         /* Current items in buffer */
    size_t          sendx;          /* Next send index */
    size_t          recvx;          /* Next recv index */

    /* State */
    bool            closed;

    /* Wait queues */
    ScWaitQ         sendq;          /* Senders blocked waiting for space */
    ScWaitQ         recvq;          /* Receivers blocked waiting for data */

    /* Lock — protects all fields above */
    pthread_mutex_t lock;
} ScChan;

/* ---- Helpers ---- */

/* Copy elem_size bytes at a buffer index */
static inline void *chan_buf_at(ScChan *ch, size_t i) {
    return (char *)ch->buf + i * ch->elem_size;
}

/* Unlock function passed to sc_park — releases channel lock */
static bool chan_unlock(void *lock) {
    pthread_mutex_unlock((pthread_mutex_t *)lock);
    return true;
}

/* ---- Channel API ---- */

void *__sc_chan_new(size_t elem_size, size_t cap) {
    ScChan *ch = calloc(1, sizeof(ScChan));
    if (!ch) { fprintf(stderr, "__sc_chan_new: out of memory\n"); abort(); }

    ch->elem_size = elem_size;
    ch->dataqsiz = cap;

    if (cap > 0) {
        ch->buf = malloc(elem_size * cap);
        if (!ch->buf) { fprintf(stderr, "__sc_chan_new: buffer alloc failed\n"); abort(); }
    }

    ch->sendq.first = NULL;
    ch->sendq.last = NULL;
    ch->recvq.first = NULL;
    ch->recvq.last = NULL;

    pthread_mutex_init(&ch->lock, NULL);
    return ch;
}

void __sc_chan_send(ScChan *ch, void *val_ptr) {
    pthread_mutex_lock(&ch->lock);

    /* Send to closed channel: panic */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        fprintf(stderr, "panic: send on closed channel\n");
        abort();
    }

    /* Fast path 1: receiver waiting — copy directly to receiver */
    ScSudog *sg = sc_waitq_dequeue(&ch->recvq);
    if (sg) {
        /* Copy data directly to receiver's buffer */
        if (sg->elem) {
            memcpy(sg->elem, val_ptr, ch->elem_size);
        }
        sg->success = true;
        ScW *gp = sg->g;
        pthread_mutex_unlock(&ch->lock);
        /* Do NOT release sg — the receiver owns it and frees after waking */
        sc_ready(gp);
        return;
    }

    /* Fast path 2: buffer has space */
    if (ch->qcount < ch->dataqsiz) {
        memcpy(chan_buf_at(ch, ch->sendx), val_ptr, ch->elem_size);
        ch->sendx = (ch->sendx + 1) % ch->dataqsiz;
        ch->qcount++;
        pthread_mutex_unlock(&ch->lock);
        return;
    }

    /* Slow path: must block. Park this wake on the send queue. */
    ScSudog *mysg = sc_sudog_acquire();
    mysg->g = sc_getw();
    mysg->elem = val_ptr;  /* Receiver will copy from here */
    mysg->success = false;
    mysg->is_select = false;
    sc_waitq_enqueue(&ch->sendq, mysg);

    /* Park — releases channel lock atomically after parking */
    sc_park(chan_unlock, &ch->lock);

    /* Resumed — the receiver copied our data (or channel was closed) */
    bool success = mysg->success;
    sc_sudog_release(mysg);

    if (!success) {
        /* Woken because channel closed */
        fprintf(stderr, "panic: send on closed channel\n");
        abort();
    }
}

/* Receive with a success flag (for closed channel detection).
 * Returns true if a value was received, false if channel is closed and empty. */
static bool chan_recv_impl(ScChan *ch, void *out_ptr) {
    pthread_mutex_lock(&ch->lock);

    /* Fast path 1: sender waiting (unbuffered or buffer full with waiting sender) */
    ScSudog *sg = sc_waitq_dequeue(&ch->sendq);
    if (sg) {
        if (ch->dataqsiz == 0) {
            /* Unbuffered: copy directly from sender's buffer */
            if (out_ptr && sg->elem) {
                memcpy(out_ptr, sg->elem, ch->elem_size);
            }
        } else {
            /* Buffered with full buffer and waiting sender:
             * 1. Copy buffer[recvx] to receiver
             * 2. Copy sender's data to buffer[recvx] (the slot we just freed)
             * 3. Advance recvx and sendx */
            if (out_ptr) {
                memcpy(out_ptr, chan_buf_at(ch, ch->recvx), ch->elem_size);
            }
            if (sg->elem) {
                memcpy(chan_buf_at(ch, ch->recvx), sg->elem, ch->elem_size);
            }
            ch->recvx = (ch->recvx + 1) % ch->dataqsiz;
            ch->sendx = ch->recvx;  /* Buffer still full, sendx follows recvx */
        }
        sg->success = true;
        ScW *gp = sg->g;
        pthread_mutex_unlock(&ch->lock);
        /* Do NOT release sg — the sender owns it and frees after waking */
        sc_ready(gp);
        return true;
    }

    /* Fast path 2: buffer has data */
    if (ch->qcount > 0) {
        if (out_ptr) {
            memcpy(out_ptr, chan_buf_at(ch, ch->recvx), ch->elem_size);
        }
        ch->recvx = (ch->recvx + 1) % ch->dataqsiz;
        ch->qcount--;
        pthread_mutex_unlock(&ch->lock);
        return true;
    }

    /* Channel empty and closed: return zero value */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        if (out_ptr) {
            memset(out_ptr, 0, ch->elem_size);
        }
        return false;
    }

    /* Slow path: must block. Park on the recv queue. */
    ScSudog *mysg = sc_sudog_acquire();
    mysg->g = sc_getw();
    mysg->elem = out_ptr;  /* Sender will copy into here */
    mysg->success = false;
    mysg->is_select = false;
    sc_waitq_enqueue(&ch->recvq, mysg);

    /* Park — releases channel lock atomically */
    sc_park(chan_unlock, &ch->lock);

    /* Resumed */
    bool success = mysg->success;
    sc_sudog_release(mysg);
    return success;
}

void __sc_chan_recv(ScChan *ch, void *out_ptr) {
    chan_recv_impl(ch, out_ptr);
}

/* Receive with ok flag — returns 1 if value received, 0 if closed.
 * For future `val, ok := <-ch` support. */
int __sc_chan_recv_ok(ScChan *ch, void *out_ptr) {
    return chan_recv_impl(ch, out_ptr) ? 1 : 0;
}

void __sc_chan_close(ScChan *ch) {
    pthread_mutex_lock(&ch->lock);

    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        fprintf(stderr, "panic: close of closed channel\n");
        abort();
    }

    ch->closed = true;

    /* Wake all receivers — they'll get zero values.
     * Do NOT free sudogs — the receivers own them. */
    ScSudog *sg;
    while ((sg = sc_waitq_dequeue(&ch->recvq)) != NULL) {
        sg->success = false;
        if (sg->elem) {
            memset(sg->elem, 0, ch->elem_size);
        }
        ScW *gp = sg->g;
        sc_ready(gp);
    }

    /* Wake all senders — they'll panic.
     * Do NOT free sudogs — the senders own them. */
    while ((sg = sc_waitq_dequeue(&ch->sendq)) != NULL) {
        sg->success = false;
        ScW *gp = sg->g;
        sc_ready(gp);
    }

    pthread_mutex_unlock(&ch->lock);
}

size_t __sc_chan_len(ScChan *ch) {
    pthread_mutex_lock(&ch->lock);
    size_t len = ch->qcount;
    pthread_mutex_unlock(&ch->lock);
    return len;
}

size_t __sc_chan_cap(ScChan *ch) {
    return ch->dataqsiz;
}

/* ---- WaitGroup ---- */

typedef struct {
    _Atomic int64_t counter;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} ScWaitGroup;

void *__sc_wg_new(void) {
    ScWaitGroup *wg = calloc(1, sizeof(ScWaitGroup));
    atomic_store(&wg->counter, 0);
    pthread_mutex_init(&wg->mu, NULL);
    pthread_cond_init(&wg->cv, NULL);
    return wg;
}

void __sc_wg_add(ScWaitGroup *wg, int64_t delta) {
    int64_t new_val = atomic_fetch_add(&wg->counter, delta) + delta;
    if (new_val < 0) {
        fprintf(stderr, "panic: WaitGroup counter went negative\n");
        abort();
    }
    if (new_val == 0) {
        pthread_mutex_lock(&wg->mu);
        pthread_cond_broadcast(&wg->cv);
        pthread_mutex_unlock(&wg->mu);
    }
}

void __sc_wg_done(ScWaitGroup *wg) {
    __sc_wg_add(wg, -1);
}

void __sc_wg_wait(ScWaitGroup *wg) {
    while (atomic_load(&wg->counter) > 0) {
        pthread_mutex_lock(&wg->mu);
        if (atomic_load(&wg->counter) > 0) {
            pthread_cond_wait(&wg->cv, &wg->mu);
        }
        pthread_mutex_unlock(&wg->mu);
    }
}

/* ---- Mutex<T> ---- */
/* Scheduler-aware mutual exclusion. Lock() parks the wake instead of
 * blocking the OS thread. Uses sudog wait queue like channels.
 *
 * The data pointer is stored alongside the lock so Mutex<T> wraps T.
 * Lock() returns an opaque ptr to the data — the caller accesses it
 * while holding the lock, then calls Unlock() to release.
 *
 * Layout: { pthread_mutex_t lock, ScWaitQ waiters, bool locked, void *data, size_t data_size }
 */

typedef struct {
    pthread_mutex_t mu;         /* Protects locked + waiters */
    ScWaitQ         waiters;    /* Wakes blocked on Lock() */
    bool            locked;     /* Is the mutex currently held? */
    void           *data;       /* Pointer to the guarded value */
    size_t          data_size;  /* Size of the guarded value */
} ScMutex;

/* Unlock function for sc_park — releases the mutex's internal lock */
static bool mutex_unlock(void *lock) {
    pthread_mutex_unlock((pthread_mutex_t *)lock);
    return true;
}

void *__sc_mutex_new(size_t elem_size) {
    ScMutex *m = calloc(1, sizeof(ScMutex));
    if (!m) { fprintf(stderr, "__sc_mutex_new: out of memory\n"); abort(); }
    pthread_mutex_init(&m->mu, NULL);
    m->waiters.first = NULL;
    m->waiters.last = NULL;
    m->locked = false;
    m->data = calloc(1, elem_size);
    if (!m->data) { fprintf(stderr, "__sc_mutex_new: data alloc failed\n"); abort(); }
    m->data_size = elem_size;
    return m;
}

void *__sc_mutex_new_with(void *init_val, size_t elem_size) {
    ScMutex *m = calloc(1, sizeof(ScMutex));
    if (!m) { fprintf(stderr, "__sc_mutex_new_with: out of memory\n"); abort(); }
    pthread_mutex_init(&m->mu, NULL);
    m->waiters.first = NULL;
    m->waiters.last = NULL;
    m->locked = false;
    m->data = malloc(elem_size);
    if (!m->data) { fprintf(stderr, "__sc_mutex_new_with: data alloc failed\n"); abort(); }
    memcpy(m->data, init_val, elem_size);
    m->data_size = elem_size;
    return m;
}

/* Lock — acquire the mutex. Parks the wake if contended. */
void *__sc_mutex_lock(ScMutex *m) {
    pthread_mutex_lock(&m->mu);

    if (!m->locked) {
        /* Fast path: uncontended */
        m->locked = true;
        pthread_mutex_unlock(&m->mu);
        return m->data;
    }

    /* Slow path: contended — park this wake on the wait queue */
    ScSudog *sg = sc_sudog_acquire();
    sg->g = sc_getw();
    sg->success = false;
    sg->is_select = false;
    sc_waitq_enqueue(&m->waiters, sg);

    /* Park — releases mutex lock atomically */
    sc_park(mutex_unlock, &m->mu);

    /* Resumed — we now hold the mutex */
    sc_sudog_release(sg);
    return m->data;
}

/* TryLock — non-blocking attempt. Returns data ptr or NULL. */
void *__sc_mutex_trylock(ScMutex *m) {
    pthread_mutex_lock(&m->mu);
    if (!m->locked) {
        m->locked = true;
        pthread_mutex_unlock(&m->mu);
        return m->data;
    }
    pthread_mutex_unlock(&m->mu);
    return NULL;
}

/* Unlock — release the mutex. Wakes one waiting wake if any. */
/* ---- Once ---- */
/* Run initialization exactly once. Thread-safe via atomic flag + mutex. */

typedef struct {
    _Atomic int32_t done;       /* 0 = not done, 1 = done */
    pthread_mutex_t mu;
} ScOnce;

void *__sc_once_new(void) {
    ScOnce *o = calloc(1, sizeof(ScOnce));
    if (!o) { fprintf(stderr, "__sc_once_new: out of memory\n"); abort(); }
    atomic_store(&o->done, 0);
    pthread_mutex_init(&o->mu, NULL);
    return o;
}

/* Do — run fn exactly once. All callers block until the first completes. */
void __sc_once_do(ScOnce *o, void (*fn)(void *), void *arg) {
    if (atomic_load_explicit(&o->done, memory_order_acquire)) return;
    pthread_mutex_lock(&o->mu);
    if (!atomic_load(&o->done)) {
        fn(arg);
        atomic_store_explicit(&o->done, 1, memory_order_release);
    }
    pthread_mutex_unlock(&o->mu);
}

/* ---- Barrier ---- */
/* Synchronization point — all wakes must arrive before any proceed. */

typedef struct {
    int32_t         threshold;
    _Atomic int32_t count;
    _Atomic int32_t generation;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} ScBarrier;

void *__sc_barrier_new(int32_t count) {
    ScBarrier *b = calloc(1, sizeof(ScBarrier));
    if (!b) { fprintf(stderr, "__sc_barrier_new: out of memory\n"); abort(); }
    b->threshold = count;
    atomic_store(&b->count, 0);
    atomic_store(&b->generation, 0);
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    return b;
}

/* Wait — block until all participants arrive. Returns true for the last arriver. */
bool __sc_barrier_wait(ScBarrier *b) {
    pthread_mutex_lock(&b->mu);
    int32_t gen = atomic_load(&b->generation);
    int32_t n = atomic_fetch_add(&b->count, 1) + 1;

    if (n == b->threshold) {
        /* Last arriver — reset and wake everyone */
        atomic_store(&b->count, 0);
        atomic_fetch_add(&b->generation, 1);
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mu);
        return true;
    }

    /* Wait for this generation to complete */
    while (atomic_load(&b->generation) == gen) {
        pthread_cond_wait(&b->cv, &b->mu);
    }
    pthread_mutex_unlock(&b->mu);
    return false;
}

void __sc_mutex_unlock(ScMutex *m) {
    pthread_mutex_lock(&m->mu);

    ScSudog *sg = sc_waitq_dequeue(&m->waiters);
    if (sg) {
        /* Hand off directly to the next waiter — mutex stays locked */
        ScW *gp = sg->g;
        pthread_mutex_unlock(&m->mu);
        sc_ready(gp);
    } else {
        /* No waiters — unlock */
        m->locked = false;
        pthread_mutex_unlock(&m->mu);
    }
}

/* ---- RwLock<T> ---- */
/* Multiple readers OR one writer. Scheduler-aware.
 * Writers wait for all readers to release. Readers wait if a writer holds.
 * Uses the same park/ready pattern as Mutex. */

typedef struct {
    pthread_mutex_t mu;
    ScWaitQ         readers_q;   /* Writers waiting for readers to finish */
    ScWaitQ         writers_q;   /* Readers/writers waiting for writer to finish */
    int32_t         reader_count; /* Active readers (0 when writer holds) */
    bool            writer_held;  /* Is a writer holding the lock? */
    void           *data;
    size_t          data_size;
} ScRwLock;

static bool rwlock_unlock(void *lock) {
    pthread_mutex_unlock((pthread_mutex_t *)lock);
    return true;
}

void *__sc_rwlock_new(size_t elem_size) {
    ScRwLock *rw = calloc(1, sizeof(ScRwLock));
    if (!rw) { fprintf(stderr, "__sc_rwlock_new: out of memory\n"); abort(); }
    pthread_mutex_init(&rw->mu, NULL);
    rw->readers_q.first = rw->readers_q.last = NULL;
    rw->writers_q.first = rw->writers_q.last = NULL;
    rw->reader_count = 0;
    rw->writer_held = false;
    rw->data = calloc(1, elem_size);
    if (!rw->data) { fprintf(stderr, "__sc_rwlock_new: data alloc failed\n"); abort(); }
    rw->data_size = elem_size;
    return rw;
}

void *__sc_rwlock_new_with(void *init_val, size_t elem_size) {
    ScRwLock *rw = calloc(1, sizeof(ScRwLock));
    if (!rw) { fprintf(stderr, "__sc_rwlock_new_with: out of memory\n"); abort(); }
    pthread_mutex_init(&rw->mu, NULL);
    rw->readers_q.first = rw->readers_q.last = NULL;
    rw->writers_q.first = rw->writers_q.last = NULL;
    rw->reader_count = 0;
    rw->writer_held = false;
    rw->data = malloc(elem_size);
    if (!rw->data) { fprintf(stderr, "__sc_rwlock_new_with: data alloc failed\n"); abort(); }
    memcpy(rw->data, init_val, elem_size);
    rw->data_size = elem_size;
    return rw;
}

/* RLock — acquire read lock. Multiple readers allowed. */
void *__sc_rwlock_rlock(ScRwLock *rw) {
    pthread_mutex_lock(&rw->mu);

    while (rw->writer_held) {
        /* Writer active — park until writer releases */
        ScSudog *sg = sc_sudog_acquire();
        sg->g = sc_getw();
        sg->success = false;
        sg->is_select = false;
        sc_waitq_enqueue(&rw->writers_q, sg);
        sc_park(rwlock_unlock, &rw->mu);
        sc_sudog_release(sg);
        pthread_mutex_lock(&rw->mu);
    }

    rw->reader_count++;
    pthread_mutex_unlock(&rw->mu);
    return rw->data;
}

/* RUnlock — release read lock. Wakes a waiting writer if last reader. */
void __sc_rwlock_runlock(ScRwLock *rw) {
    pthread_mutex_lock(&rw->mu);
    rw->reader_count--;

    if (rw->reader_count == 0) {
        /* Last reader — wake one waiting writer if any */
        ScSudog *sg = sc_waitq_dequeue(&rw->readers_q);
        if (sg) {
            ScW *gp = sg->g;
            pthread_mutex_unlock(&rw->mu);
            sc_ready(gp);
            return;
        }
    }
    pthread_mutex_unlock(&rw->mu);
}

/* WLock — acquire write lock. Exclusive access. */
void *__sc_rwlock_wlock(ScRwLock *rw) {
    pthread_mutex_lock(&rw->mu);

    while (rw->writer_held || rw->reader_count > 0) {
        /* Either writer active or readers active — park */
        ScSudog *sg = sc_sudog_acquire();
        sg->g = sc_getw();
        sg->success = false;
        sg->is_select = false;
        if (rw->writer_held) {
            sc_waitq_enqueue(&rw->writers_q, sg);
        } else {
            sc_waitq_enqueue(&rw->readers_q, sg);
        }
        sc_park(rwlock_unlock, &rw->mu);
        sc_sudog_release(sg);
        pthread_mutex_lock(&rw->mu);
    }

    rw->writer_held = true;
    pthread_mutex_unlock(&rw->mu);
    return rw->data;
}

/* WUnlock — release write lock. Wakes all waiting readers, or one writer. */
void __sc_rwlock_wunlock(ScRwLock *rw) {
    pthread_mutex_lock(&rw->mu);
    rw->writer_held = false;

    /* Prefer waking readers (multiple can proceed) */
    ScSudog *sg;
    bool woke_any = false;
    while ((sg = sc_waitq_dequeue(&rw->writers_q)) != NULL) {
        ScW *gp = sg->g;
        sc_ready(gp);
        woke_any = true;
    }
    if (!woke_any) {
        /* No readers waiting — wake one writer */
        sg = sc_waitq_dequeue(&rw->readers_q);
        if (sg) {
            ScW *gp = sg->g;
            sc_ready(gp);
        }
    }
    pthread_mutex_unlock(&rw->mu);
}

/* ---- Select ---- */
/* Channel multiplexing. Simplified implementation:
 * 1. Lock all channels
 * 2. Poll: check if any case can proceed immediately
 * 3. If yes: execute it, unlock all, return case index
 * 4. If no and has default: unlock all, return default index
 * 5. If no and no default: enqueue sudog on all channels, park, wake when one fires
 *
 * Case struct passed from codegen:
 *   { int direction, ScChan *chan, void *data }
 *   direction: 0 = recv, 1 = send, 2 = default
 */

typedef struct {
    int     dir;    /* 0=recv, 1=send, 2=default */
    void   *chan;   /* ScChan* (NULL for default) */
    void   *data;   /* ptr to recv buffer or send value */
} ScSelectCase;

int __sc_select(ScSelectCase *cases, int ncase) {
    /* Find default index (-1 if none) */
    int default_idx = -1;
    for (int i = 0; i < ncase; i++) {
        if (cases[i].dir == 2) { default_idx = i; break; }
    }

    /* Lock all channels (sorted by address to avoid deadlock) */
    /* Simple approach: just lock in order. For correctness with few channels
     * (typical select has 2-4 cases) this is fine. */
    for (int i = 0; i < ncase; i++) {
        if (cases[i].chan)
            pthread_mutex_lock(&((ScChan *)cases[i].chan)->lock);
    }

    /* Poll: check if any case can proceed */
    for (int i = 0; i < ncase; i++) {
        ScChan *ch = (ScChan *)cases[i].chan;
        if (!ch) continue;

        if (cases[i].dir == 0) {
            /* Recv: can proceed if buffer has data OR sender waiting */
            if (ch->qcount > 0 || ch->sendq.first != NULL || ch->closed) {
                /* Unlock all channels */
                for (int j = 0; j < ncase; j++)
                    if (cases[j].chan)
                        pthread_mutex_unlock(&((ScChan *)cases[j].chan)->lock);
                /* Do the recv outside locks */
                if (cases[i].data)
                    __sc_chan_recv(ch, cases[i].data);
                return i;
            }
        } else if (cases[i].dir == 1) {
            /* Send: can proceed if buffer has space OR receiver waiting */
            if (ch->closed) {
                for (int j = 0; j < ncase; j++)
                    if (cases[j].chan)
                        pthread_mutex_unlock(&((ScChan *)cases[j].chan)->lock);
                fprintf(stderr, "panic: send on closed channel in select\n");
                abort();
            }
            if (ch->qcount < ch->dataqsiz || ch->recvq.first != NULL) {
                for (int j = 0; j < ncase; j++)
                    if (cases[j].chan)
                        pthread_mutex_unlock(&((ScChan *)cases[j].chan)->lock);
                __sc_chan_send(ch, cases[i].data);
                return i;
            }
        }
    }

    /* No case ready */
    if (default_idx >= 0) {
        for (int i = 0; i < ncase; i++)
            if (cases[i].chan)
                pthread_mutex_unlock(&((ScChan *)cases[i].chan)->lock);
        return default_idx;
    }

    /* No default — must block. Park on all channels.
     * Use a shared flag to detect which case fired. */
    /* chosen is determined after wake by checking sudog->success */
    ScSudog **sudogs = alloca(sizeof(ScSudog *) * ncase);

    for (int i = 0; i < ncase; i++) {
        ScChan *ch = (ScChan *)cases[i].chan;
        if (!ch) { sudogs[i] = NULL; continue; }

        ScSudog *sg = sc_sudog_acquire();
        sg->g = sc_getw();
        sg->elem = cases[i].data;
        sg->success = false;
        sg->is_select = true;
        sudogs[i] = sg;

        if (cases[i].dir == 0)
            sc_waitq_enqueue(&ch->recvq, sg);
        else
            sc_waitq_enqueue(&ch->sendq, sg);
    }

    /* Unlock all channels and park */
    for (int i = 0; i < ncase; i++)
        if (cases[i].chan)
            pthread_mutex_unlock(&((ScChan *)cases[i].chan)->lock);

    /* Park — will be woken when any channel operation completes.
     * We use a simple approach: park with no unlock function,
     * the channel send/recv will call sc_ready on our sudog. */
    sc_park(NULL, NULL);

    /* Woken — find which case fired */
    int result = -1;
    for (int i = 0; i < ncase; i++) {
        if (sudogs[i] && sudogs[i]->success) {
            result = i;
        }
    }

    /* Dequeue our sudogs from channels we didn't win */
    for (int i = 0; i < ncase; i++) {
        if (!sudogs[i]) continue;
        if (i != result) {
            ScChan *ch = (ScChan *)cases[i].chan;
            pthread_mutex_lock(&ch->lock);
            /* Remove from wait queue */
            ScSudog *sg = sudogs[i];
            if (sg->prev) sg->prev->next = sg->next;
            if (sg->next) sg->next->prev = sg->prev;
            if (cases[i].dir == 0) {
                if (ch->recvq.first == sg) ch->recvq.first = sg->next;
                if (ch->recvq.last == sg) ch->recvq.last = sg->prev;
            } else {
                if (ch->sendq.first == sg) ch->sendq.first = sg->next;
                if (ch->sendq.last == sg) ch->sendq.last = sg->prev;
            }
            pthread_mutex_unlock(&ch->lock);
        }
        sc_sudog_release(sudogs[i]);
    }

    return result >= 0 ? result : 0;
}

/* ---- TaskScope ---- */
/* Structured concurrency: all spawned wakes complete before scope exits.
 * Internally a WaitGroup with Spawn that does Add(1) + wake + Done(). */

typedef struct {
    ScWaitGroup wg;
} ScTaskScope;

void *__sc_taskscope_new(void) {
    ScTaskScope *ts = calloc(1, sizeof(ScTaskScope));
    if (!ts) { fprintf(stderr, "__sc_taskscope_new: out of memory\n"); abort(); }
    atomic_store(&ts->wg.counter, 0);
    pthread_mutex_init(&ts->wg.mu, NULL);
    pthread_cond_init(&ts->wg.cv, NULL);
    return ts;
}

/* Spawn wrapper: calls fn(arg), then Done() on the scope's WaitGroup */
typedef struct {
    void (*fn)(void *);
    void *arg;
    ScTaskScope *scope;
} ScSpawnCtx;

static void taskscope_spawn_wrapper(void *ctx_ptr) {
    ScSpawnCtx *ctx = (ScSpawnCtx *)ctx_ptr;
    ctx->fn(ctx->arg);
    __sc_wg_done(&ctx->scope->wg);
    free(ctx);
}

/* Spawn(fn, arg) — add wake to scope, auto-done when complete */
void __sc_taskscope_spawn(ScTaskScope *ts, void (*fn)(void *), void *arg) {
    __sc_wg_add(&ts->wg, 1);
    ScSpawnCtx *ctx = malloc(sizeof(ScSpawnCtx));
    if (!ctx) { fprintf(stderr, "__sc_taskscope_spawn: out of memory\n"); abort(); }
    ctx->fn = fn;
    ctx->arg = arg;
    ctx->scope = ts;
    __sc_wake_spawn(taskscope_spawn_wrapper, ctx);
}

/* Wait — block until all spawned wakes complete */
void __sc_taskscope_wait(ScTaskScope *ts) {
    __sc_wg_wait(&ts->wg);
}

/* ---- Broadcast Channel ---- */
/* One sender, multiple receivers. Each receiver sees every message.
 * Ring buffer with per-subscriber read cursors.
 * Subscribers that fall behind (cursor < write_pos - cap) lose messages. */

typedef struct ScBroadcastSub ScBroadcastSub;

typedef struct {
    pthread_mutex_t mu;
    void           *buf;          /* Ring buffer */
    size_t          elem_size;
    size_t          cap;          /* Buffer capacity */
    size_t          write_pos;    /* Monotonic write position */
    bool            closed;
    ScBroadcastSub *subs;         /* Linked list of subscribers */
} ScBroadcast;

struct ScBroadcastSub {
    ScBroadcast    *parent;
    size_t          read_pos;     /* This subscriber's read cursor */
    pthread_mutex_t mu;
    pthread_cond_t  cv;           /* Signaled when new data arrives */
    ScBroadcastSub *next;
};

void *__sc_broadcast_new(size_t elem_size, size_t cap) {
    if (cap == 0) cap = 16; /* minimum buffer */
    ScBroadcast *b = calloc(1, sizeof(ScBroadcast));
    if (!b) { fprintf(stderr, "__sc_broadcast_new: out of memory\n"); abort(); }
    pthread_mutex_init(&b->mu, NULL);
    b->buf = malloc(elem_size * cap);
    if (!b->buf) { fprintf(stderr, "__sc_broadcast_new: buf alloc failed\n"); abort(); }
    b->elem_size = elem_size;
    b->cap = cap;
    b->write_pos = 0;
    b->closed = false;
    b->subs = NULL;
    return b;
}

void *__sc_broadcast_subscribe(ScBroadcast *b) {
    ScBroadcastSub *sub = calloc(1, sizeof(ScBroadcastSub));
    if (!sub) { fprintf(stderr, "__sc_broadcast_subscribe: out of memory\n"); abort(); }
    sub->parent = b;
    pthread_mutex_init(&sub->mu, NULL);
    pthread_cond_init(&sub->cv, NULL);

    pthread_mutex_lock(&b->mu);
    sub->read_pos = b->write_pos; /* Start from current position */
    sub->next = b->subs;
    b->subs = sub;
    pthread_mutex_unlock(&b->mu);
    return sub;
}

void __sc_broadcast_send(ScBroadcast *b, void *val_ptr) {
    pthread_mutex_lock(&b->mu);
    if (b->closed) {
        pthread_mutex_unlock(&b->mu);
        fprintf(stderr, "panic: send on closed broadcast channel\n");
        abort();
    }

    /* Write to ring buffer */
    size_t idx = b->write_pos % b->cap;
    memcpy((char *)b->buf + idx * b->elem_size, val_ptr, b->elem_size);
    b->write_pos++;

    /* Wake all subscribers */
    for (ScBroadcastSub *s = b->subs; s; s = s->next) {
        pthread_mutex_lock(&s->mu);
        pthread_cond_signal(&s->cv);
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&b->mu);
}

bool __sc_broadcast_recv(ScBroadcastSub *sub, void *out_ptr) {
    ScBroadcast *b = sub->parent;

    pthread_mutex_lock(&sub->mu);
    while (1) {
        pthread_mutex_lock(&b->mu);
        if (sub->read_pos < b->write_pos) {
            /* Data available */
            size_t idx = sub->read_pos % b->cap;
            if (out_ptr)
                memcpy(out_ptr, (char *)b->buf + idx * b->elem_size, b->elem_size);
            sub->read_pos++;
            pthread_mutex_unlock(&b->mu);
            pthread_mutex_unlock(&sub->mu);
            return true;
        }
        if (b->closed) {
            pthread_mutex_unlock(&b->mu);
            pthread_mutex_unlock(&sub->mu);
            if (out_ptr) memset(out_ptr, 0, b->elem_size);
            return false;
        }
        pthread_mutex_unlock(&b->mu);
        /* Wait for signal */
        pthread_cond_wait(&sub->cv, &sub->mu);
    }
}

void __sc_broadcast_close(ScBroadcast *b) {
    pthread_mutex_lock(&b->mu);
    b->closed = true;
    for (ScBroadcastSub *s = b->subs; s; s = s->next) {
        pthread_mutex_lock(&s->mu);
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&b->mu);
}

/* ---- Oneshot Channel ---- */
/* Single-use channel: capacity 1, auto-closes after first send.
 * Just a regular channel with cap=1 + auto-close on send. */

void __sc_chan_send_oneshot(ScChan *ch, void *val_ptr) {
    __sc_chan_send(ch, val_ptr);
    __sc_chan_close(ch);
}

/* ---- Actor ---- */
/* Isolated state + message inbox. Processes one message at a time.
 * Runs a handler function in a loop on its own wake.
 *
 * Layout: channel for inbox + handler function + initial state.
 * The handler receives (state_ptr, msg_ptr) and can mutate state.
 * No external access to state — only the handler can touch it. */

typedef struct {
    void  *inbox;       /* ScChan* for messages */
    void  *state;       /* Actor's private state */
    size_t state_size;
    size_t msg_size;
    void (*handler)(void *state, void *msg);
} ScActor;

static void actor_loop(void *arg) {
    ScActor *a = (ScActor *)arg;
    void *msg_buf = malloc(a->msg_size);
    if (!msg_buf) { fprintf(stderr, "actor_loop: out of memory\n"); abort(); }

    while (1) {
        int ok = __sc_chan_recv_ok((ScChan *)a->inbox, msg_buf);
        if (!ok) break; /* Channel closed — actor shuts down */
        a->handler(a->state, msg_buf);
    }

    free(msg_buf);
    free(a->state);
    free(a);
}

void *__sc_actor_new(void *init_state, size_t state_size,
                     size_t msg_size, size_t inbox_cap,
                     void (*handler)(void *, void *)) {
    ScActor *a = malloc(sizeof(ScActor));
    if (!a) { fprintf(stderr, "__sc_actor_new: out of memory\n"); abort(); }

    a->inbox = __sc_chan_new(msg_size, inbox_cap > 0 ? inbox_cap : 64);
    a->state = malloc(state_size);
    if (!a->state) { fprintf(stderr, "__sc_actor_new: state alloc failed\n"); abort(); }
    memcpy(a->state, init_state, state_size);
    a->state_size = state_size;
    a->msg_size = msg_size;
    a->handler = handler;

    /* Spawn the actor loop on its own wake */
    __sc_wake_spawn(actor_loop, a);
    return a->inbox; /* Return the inbox channel — callers send messages here */
}

/* Send message to actor — just send on its inbox channel */
void __sc_actor_send(void *inbox, void *msg_ptr) {
    __sc_chan_send((ScChan *)inbox, msg_ptr);
}

/* Stop actor — close the inbox channel */
void __sc_actor_stop(void *inbox) {
    __sc_chan_close((ScChan *)inbox);
}

/* ---- Future (async/await) ---- */
/* A Future<T> is just a chan<T> with capacity 1.
 * async spawns a wake that sends the result to the future.
 * await receives from the future channel.
 * The runtime functions are trivial wrappers. */

void *__sc_future_new(size_t elem_size) {
    return __sc_chan_new(elem_size, 1);
}

void __sc_future_complete(void *future, void *val_ptr) {
    __sc_chan_send((ScChan *)future, val_ptr);
}

void __sc_future_await(void *future, void *out_ptr) {
    __sc_chan_recv((ScChan *)future, out_ptr);
}

/* ---- Atomic<T> ---- */
/* Lock-free atomics for integer types. Uses C11 _Atomic.
 * All operations use i64 internally — callers sign-extend/truncate as needed.
 * Heap-allocated so multiple wakes can share the same atomic. */

typedef struct {
    _Atomic int64_t val;
} ScAtomic;

void *__sc_atomic_new(int64_t init) {
    ScAtomic *a = calloc(1, sizeof(ScAtomic));
    if (!a) { fprintf(stderr, "__sc_atomic_new: out of memory\n"); abort(); }
    atomic_store(&a->val, init);
    return a;
}

int64_t __sc_atomic_load(ScAtomic *a) {
    return atomic_load_explicit(&a->val, memory_order_seq_cst);
}

void __sc_atomic_store(ScAtomic *a, int64_t val) {
    atomic_store_explicit(&a->val, val, memory_order_seq_cst);
}

int64_t __sc_atomic_add(ScAtomic *a, int64_t delta) {
    return atomic_fetch_add_explicit(&a->val, delta, memory_order_seq_cst);
}

int64_t __sc_atomic_swap(ScAtomic *a, int64_t val) {
    return atomic_exchange_explicit(&a->val, val, memory_order_seq_cst);
}

bool __sc_atomic_cas(ScAtomic *a, int64_t old_val, int64_t new_val) {
    return atomic_compare_exchange_strong_explicit(
        &a->val, &old_val, new_val,
        memory_order_seq_cst, memory_order_seq_cst);
}

/* ==================================================================
 * SSO String — Small String Optimization
 *
 * 24-byte tagged union. Strings ≤23 bytes are stored inline.
 * Layout (as a [24]u8 blob):
 *   Inline: bytes[0..len-1] = data, bytes[23] = len (0-23)
 *   Heap:   bytes[0..7] = ptr, bytes[8..15] = len, bytes[16..22] = cap low bytes,
 *           bytes[23] = 0xFF (tag)
 *
 * Byte 23 discriminates: < 24 means inline with that length, 0xFF means heap.
 * ================================================================== */

/* ================================================================== *
 * Command-line argument access                                        *
 * ================================================================== */

static int    __sc_stored_argc = 0;
static char **__sc_stored_argv = NULL;

void __sc_set_args(int argc, char **argv) {
    __sc_stored_argc = argc;
    __sc_stored_argv = argv;
}

int __sc_get_argc(void) {
    return __sc_stored_argc;
}

/* Returns argv[i] as a *u8 (C string pointer). NULL if out of range. */
const char *__sc_get_argv(int i) {
    if (i < 0 || i >= __sc_stored_argc) return NULL;
    return __sc_stored_argv[i];
}

/* ================================================================== *
 * SSO String                                                          *
 * ================================================================== */

#define SSO_INLINE_MAX 23
#define SSO_HEAP_TAG   0xFF
#define SSO_SIZE       24

typedef struct {
    uint8_t bytes[SSO_SIZE];
} SSOStr;

/* Create an SSO string from ptr + len.
 * Takes output pointer to avoid ABI issues with returning 24-byte structs. */
void __sc_sso_new(SSOStr *out, const char *ptr, size_t len) {
    memset(out, 0, SSO_SIZE);
    if (len <= SSO_INLINE_MAX) {
        memcpy(out->bytes, ptr, len);
        out->bytes[23] = (uint8_t)len;
    } else {
        char *heap = malloc(len + 1);
        if (!heap) { fprintf(stderr, "__sc_sso_new: out of memory\n"); abort(); }
        memcpy(heap, ptr, len);
        heap[len] = 0;  /* null-terminate for CStr compatibility */
        memcpy(out->bytes, &heap, sizeof(char *));       /* ptr at offset 0 */
        memcpy(out->bytes + 8, &len, sizeof(size_t));     /* len at offset 8 */
        size_t cap = len;
        memcpy(out->bytes + 16, &cap, sizeof(size_t));    /* cap at offset 16 */
        out->bytes[23] = SSO_HEAP_TAG;
    }
}

/* Get data pointer */
const char *__sc_sso_ptr(const SSOStr *s) {
    if (s->bytes[23] == SSO_HEAP_TAG) {
        const char *ptr;
        memcpy(&ptr, s->bytes, sizeof(char *));
        return ptr;
    }
    return (const char *)s->bytes;
}

/* Get length */
size_t __sc_sso_len(const SSOStr *s) {
    if (s->bytes[23] == SSO_HEAP_TAG) {
        size_t len;
        memcpy(&len, s->bytes + 8, sizeof(size_t));
        return len;
    }
    return s->bytes[23];
}

/* Equality */
bool __sc_sso_eq(const SSOStr *a, const SSOStr *b) {
    size_t alen = __sc_sso_len(a);
    size_t blen = __sc_sso_len(b);
    if (alen != blen) return false;
    return memcmp(__sc_sso_ptr(a), __sc_sso_ptr(b), alen) == 0;
}

/* Concatenation — output via pointer to avoid ABI issues */
void __sc_sso_concat(SSOStr *out, const SSOStr *a, const SSOStr *b) {
    size_t alen = __sc_sso_len(a);
    size_t blen = __sc_sso_len(b);
    size_t total = alen + blen;
    const char *aptr = __sc_sso_ptr(a);
    const char *bptr = __sc_sso_ptr(b);

    memset(out, 0, SSO_SIZE);

    if (total <= SSO_INLINE_MAX) {
        memcpy(out->bytes, aptr, alen);
        memcpy(out->bytes + alen, bptr, blen);
        out->bytes[23] = (uint8_t)total;
    } else {
        char *heap = malloc(total);
        if (!heap) { fprintf(stderr, "__sc_sso_concat: out of memory\n"); abort(); }
        memcpy(heap, aptr, alen);
        memcpy(heap + alen, bptr, blen);
        memcpy(out->bytes, &heap, sizeof(char *));
        memcpy(out->bytes + 8, &total, sizeof(size_t));
        size_t cap = total;
        memcpy(out->bytes + 16, &cap, sizeof(size_t));
        out->bytes[23] = SSO_HEAP_TAG;
    }
}

/* Free heap string (no-op for inline) */
void __sc_sso_free(SSOStr *s) {
    if (s->bytes[23] == SSO_HEAP_TAG) {
        char *ptr;
        memcpy(&ptr, s->bytes, sizeof(char *));
        free(ptr);
        s->bytes[23] = 0;
    }
}

/* ==================================================================
 * File I/O — read file into SSO string
 * ================================================================== */

void __sc_read_file(SSOStr *out, const SSOStr *path_sso) {
    /* Extract path from SSO string and null-terminate */
    const char *path = __sc_sso_ptr(path_sso);
    size_t path_len = __sc_sso_len(path_sso);
    char *cpath = malloc(path_len + 1);
    memcpy(cpath, path, path_len);
    cpath[path_len] = '\0';

    FILE *f = fopen(cpath, "rb");
    free(cpath);
    if (!f) { memset(out, 0, SSO_SIZE); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    __sc_sso_new(out, NULL, 0); /* init empty */
    if (sz <= SSO_INLINE_MAX) {
        memset(out, 0, SSO_SIZE);
        fread(out->bytes, 1, (size_t)sz, f);
        out->bytes[23] = (uint8_t)sz;
    } else {
        char *buf = malloc((size_t)sz);
        fread(buf, 1, (size_t)sz, f);
        memcpy(out->bytes, &buf, sizeof(char *));
        memcpy(out->bytes + 8, &sz, sizeof(size_t));
        size_t cap = (size_t)sz;
        memcpy(out->bytes + 16, &cap, sizeof(size_t));
        out->bytes[23] = SSO_HEAP_TAG;
    }
    fclose(f);
}

/* ==================================================================
 * Allocator Interface + Implementations
 *
 * ScAllocator is a vtable-based interface:
 *   { alloc_fn, free_fn, realloc_fn, ctx }
 *
 * Three implementations:
 *   1. Default (malloc/free/realloc)
 *   2. Arena (bump allocator, bulk free)
 *   3. Pool (fixed-size blocks, free list)
 * ================================================================== */

typedef struct {
    void *(*alloc)(void *ctx, size_t size);
    void  (*dealloc)(void *ctx, void *ptr);
    void *(*realloc_fn)(void *ctx, void *ptr, size_t new_size);
    void  (*destroy)(void *ctx);   /* destroy the allocator itself */
    void  *ctx;                    /* allocator-specific state */
} ScAllocator;

/* ---- Default Allocator (malloc/free) ---- */

static void *default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void default_dealloc(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static void *default_realloc(void *ctx, void *ptr, size_t new_size) {
    (void)ctx;
    return realloc(ptr, new_size);
}

static void default_destroy(void *ctx) {
    (void)ctx; /* nothing to clean up */
}

void *__sc_allocator_default(void) {
    ScAllocator *a = malloc(sizeof(ScAllocator));
    if (!a) { fprintf(stderr, "__sc_allocator_default: out of memory\n"); abort(); }
    a->alloc = default_alloc;
    a->dealloc = default_dealloc;
    a->realloc_fn = default_realloc;
    a->destroy = default_destroy;
    a->ctx = NULL;
    return a;
}

/* ---- Arena Allocator ---- */
/* Bump allocator: O(1) alloc, O(1) bulk free on destroy.
 * Grows in chunks. Free is a no-op. */

#define ARENA_CHUNK_SIZE (64 * 1024)  /* 64KB chunks */

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used;
    size_t cap;
    char data[];  /* flexible array */
} ArenaChunk;

typedef struct {
    ArenaChunk *current;
    ArenaChunk *head;
} ArenaCtx;

static ArenaChunk *arena_new_chunk(size_t min_size) {
    size_t cap = min_size > ARENA_CHUNK_SIZE ? min_size : ARENA_CHUNK_SIZE;
    ArenaChunk *c = malloc(sizeof(ArenaChunk) + cap);
    if (!c) { fprintf(stderr, "arena: out of memory\n"); abort(); }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

static void *arena_alloc_fn(void *ctx, size_t size) {
    ArenaCtx *a = (ArenaCtx *)ctx;
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (a->current->used + size > a->current->cap) {
        /* Need a new chunk */
        ArenaChunk *c = arena_new_chunk(size);
        c->next = a->current;
        a->current = c;
    }
    void *ptr = a->current->data + a->current->used;
    a->current->used += size;
    return ptr;
}

static void arena_dealloc_fn(void *ctx, void *ptr) {
    (void)ctx; (void)ptr; /* no-op — freed on destroy */
}

static void *arena_realloc_fn(void *ctx, void *ptr, size_t new_size) {
    /* Arena can't realloc in-place — just alloc new and copy.
     * Caller is responsible for knowing the old size if they need to copy. */
    (void)ptr;
    return arena_alloc_fn(ctx, new_size);
}

static void arena_destroy_fn(void *ctx) {
    ArenaCtx *a = (ArenaCtx *)ctx;
    ArenaChunk *c = a->current;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

void *__sc_arena_new(void) {
    ArenaCtx *ctx = malloc(sizeof(ArenaCtx));
    if (!ctx) { fprintf(stderr, "__sc_arena_new: out of memory\n"); abort(); }
    ctx->head = arena_new_chunk(ARENA_CHUNK_SIZE);
    ctx->current = ctx->head;

    ScAllocator *a = malloc(sizeof(ScAllocator));
    if (!a) { fprintf(stderr, "__sc_arena_new: out of memory\n"); abort(); }
    a->alloc = arena_alloc_fn;
    a->dealloc = arena_dealloc_fn;
    a->realloc_fn = arena_realloc_fn;
    a->destroy = arena_destroy_fn;
    a->ctx = ctx;
    return a;
}

/* ---- Pool Allocator ---- */
/* Fixed-size block allocator: O(1) alloc and free via free list.
 * All blocks are the same size. Good for AST nodes, etc. */

typedef struct PoolBlock {
    struct PoolBlock *next;  /* free list chain */
} PoolBlock;

typedef struct {
    void       *buf;         /* backing buffer */
    PoolBlock  *free_list;
    size_t      block_size;  /* size of each block (>= sizeof(PoolBlock)) */
    size_t      cap;         /* total blocks */
    size_t      used;
} PoolCtx;

static void *pool_alloc_fn(void *ctx, size_t size) {
    PoolCtx *p = (PoolCtx *)ctx;
    (void)size; /* pool ignores requested size — all blocks are block_size */

    if (p->free_list) {
        PoolBlock *b = p->free_list;
        p->free_list = b->next;
        p->used++;
        return b;
    }

    /* Free list empty — check if we can bump */
    if (p->used < p->cap) {
        void *ptr = (char *)p->buf + p->used * p->block_size;
        p->used++;
        return ptr;
    }

    /* Pool exhausted — fall back to malloc */
    return malloc(p->block_size);
}

static void pool_dealloc_fn(void *ctx, void *ptr) {
    PoolCtx *p = (PoolCtx *)ctx;
    /* Check if ptr is within our buffer */
    if (ptr >= p->buf && ptr < (void *)((char *)p->buf + p->cap * p->block_size)) {
        PoolBlock *b = (PoolBlock *)ptr;
        b->next = p->free_list;
        p->free_list = b;
    } else {
        /* Was a fallback malloc — just free */
        free(ptr);
    }
}

static void *pool_realloc_fn(void *ctx, void *ptr, size_t new_size) {
    (void)ctx; (void)ptr; (void)new_size;
    /* Pool blocks are fixed-size — can't realloc */
    return NULL;
}

static void pool_destroy_fn(void *ctx) {
    PoolCtx *p = (PoolCtx *)ctx;
    free(p->buf);
    free(p);
}

void *__sc_pool_new(size_t elem_size, size_t count) {
    if (elem_size < sizeof(PoolBlock)) elem_size = sizeof(PoolBlock);
    /* Align to 8 bytes */
    elem_size = (elem_size + 7) & ~(size_t)7;

    PoolCtx *ctx = malloc(sizeof(PoolCtx));
    if (!ctx) { fprintf(stderr, "__sc_pool_new: out of memory\n"); abort(); }
    ctx->buf = malloc(elem_size * count);
    if (!ctx->buf) { fprintf(stderr, "__sc_pool_new: buf alloc failed\n"); abort(); }
    ctx->free_list = NULL;
    ctx->block_size = elem_size;
    ctx->cap = count;
    ctx->used = 0;

    ScAllocator *a = malloc(sizeof(ScAllocator));
    if (!a) { fprintf(stderr, "__sc_pool_new: out of memory\n"); abort(); }
    a->alloc = pool_alloc_fn;
    a->dealloc = pool_dealloc_fn;
    a->realloc_fn = pool_realloc_fn;
    a->destroy = pool_destroy_fn;
    a->ctx = ctx;
    return a;
}

/* ---- Allocator interface methods ---- */

void *__sc_allocator_alloc(ScAllocator *a, size_t size) {
    return a->alloc(a->ctx, size);
}

void __sc_allocator_free(ScAllocator *a, void *ptr) {
    a->dealloc(a->ctx, ptr);
}

void *__sc_allocator_realloc(ScAllocator *a, void *ptr, size_t new_size) {
    return a->realloc_fn(a->ctx, ptr, new_size);
}

void __sc_allocator_destroy(ScAllocator *a) {
    a->destroy(a->ctx);
    free(a);
}

/* ==================================================================
 * String Interning
 *
 * Global hash table for string deduplication. Each unique string is
 * stored once. Returns a {ptr, len} that points into the intern table.
 * Interned strings are never freed (they live for the process lifetime).
 *
 * Critical for compiler performance — identifiers, type names, etc.
 * are compared by pointer equality instead of memcmp.
 * ================================================================== */

#define INTERN_BUCKETS 4096

typedef struct InternNode {
    struct InternNode *next;
    size_t len;
    uint32_t hash;
    char data[];  /* flexible array — the interned string bytes */
} InternNode;

static InternNode *intern_table[INTERN_BUCKETS];
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t intern_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Intern a string. Returns pointer to the interned copy.
 * Thread-safe. Pointer-stable for the lifetime of the process. */
const char *__sc_intern(const char *s, size_t len) {
    uint32_t h = intern_hash(s, len);
    uint32_t idx = h % INTERN_BUCKETS;

    /* Fast path: check without lock (benign race — may miss, retry with lock) */
    for (InternNode *n = intern_table[idx]; n; n = n->next) {
        if (n->hash == h && n->len == len && memcmp(n->data, s, len) == 0)
            return n->data;
    }

    pthread_mutex_lock(&intern_lock);
    /* Re-check under lock */
    for (InternNode *n = intern_table[idx]; n; n = n->next) {
        if (n->hash == h && n->len == len && memcmp(n->data, s, len) == 0) {
            pthread_mutex_unlock(&intern_lock);
            return n->data;
        }
    }

    /* Not found — create new entry */
    InternNode *node = malloc(sizeof(InternNode) + len + 1);
    if (!node) { fprintf(stderr, "__sc_intern: out of memory\n"); abort(); }
    node->hash = h;
    node->len = len;
    memcpy(node->data, s, len);
    node->data[len] = '\0';
    node->next = intern_table[idx];
    intern_table[idx] = node;

    pthread_mutex_unlock(&intern_lock);
    return node->data;
}

/* Check if two interned strings are equal (pointer comparison) */
bool __sc_intern_eq(const char *a, const char *b) {
    return a == b;
}

/* Get length of an interned string (walk back to node header) */
size_t __sc_intern_len(const char *s) {
    /* The InternNode is right before the data — walk back */
    InternNode *node = (InternNode *)((char *)s - offsetof(InternNode, data));
    return node->len;
}
