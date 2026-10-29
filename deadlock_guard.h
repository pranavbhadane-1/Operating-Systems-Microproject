/*
 * deadlock_guard.h - In-process runtime deadlock detection & resolution
 *                    engine for POSIX threads.
 *
 * Model
 * -----
 *   * Every thread that touches a dg_mutex_t is lazily mapped to a dense
 *     index [0, DG_MAX_THREADS).
 *   * The Wait-For Graph (WFG) is an N x N adjacency matrix:
 *         wfg[a][b] == 1   <=>   thread a waits for a mutex held by thread b
 *   * Before a thread blocks on a held mutex, the edge is added
 *     speculatively and a DFS looks for a cycle through the caller.
 *     On a cycle the edge is rolled back and EDEADLK is returned
 *     (nothing is blocked, nothing is leaked).
 *
 * Lock hierarchy (prevents meta-deadlock)
 * ---------------------------------------
 *   Level 0 : user mutexes (dg_mutex_t.real)  - may block, may be held long
 *   Level 1 : detector->meta_mutex            - leaf lock
 *
 *   Rule 1: meta_mutex is NEVER held while blocking on a user mutex.
 *   Rule 2: meta_mutex is only held for short, non-blocking critical
 *           sections (graph updates, DFS, stderr diagnostics).
 *   Rule 3: pthread_mutex_unlock() on a user mutex happens strictly after
 *           the meta bookkeeping (ownership is cleared first, so no other
 *           thread can observe stale ownership).
 *   Hence "user -> meta" is the only nesting order that ever occurs and
 *   the detector itself can never be part of a deadlock cycle.
 *
 * Limitations (documented, deliberate)
 * ------------------------------------
 *   * Mutexes are non-recursive; re-locking a mutex you own returns EDEADLK.
 *   * All dg_mutex_t that interact must be used with ONE detector instance.
 *   * Only dg_mutex_t locks are tracked; plain pthread mutexes are invisible.
 */
#ifndef DEADLOCK_GUARD_H
#define DEADLOCK_GUARD_H

#include <errno.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DG_MAX_THREADS
#define DG_MAX_THREADS 64
#endif
/* Name used in the specification. */
#ifndef MAX_THREADS
#define MAX_THREADS DG_MAX_THREADS
#endif

/* Return codes: plain POSIX errno values.
 *   0          success
 *   EDEADLK    acquiring the lock would close a cycle in the WFG
 *   EINVAL     NULL / uninitialised object
 *   EAGAIN     thread registry full (more than DG_MAX_THREADS live threads)
 *   EPERM      unlock by a thread that does not own the mutex
 *   EBUSY      destroy while the object is still in use
 */
#define DG_OK        0
#define DG_EDEADLK   EDEADLK

typedef struct dg_mutex dg_mutex_t;

typedef struct {
    int         in_use;      /* slot allocated?                         */
    pthread_t   tid;         /* real thread id mapped to this slot      */
    int         held_count;  /* number of dg_mutex_t currently owned    */
    dg_mutex_t *waiting_on;  /* mutex this thread is blocked on, if any */
} dg_thread_slot_t;

typedef struct deadlock_detector {
    pthread_mutex_t  meta_mutex;                 /* guards EVERYTHING below */
    int              initialized;
    int              verbose;                    /* print alerts to stderr  */
    int              thread_count;
    dg_thread_slot_t threads[DG_MAX_THREADS];    /* registry                */
    unsigned char    wfg[DG_MAX_THREADS][DG_MAX_THREADS]; /* T_wait -> T_hold */
    unsigned long    deadlocks_detected;
} deadlock_detector_t;

struct dg_mutex {
    pthread_mutex_t real;        /* the actual lock                         */
    int             id;          /* user-visible id, used in diagnostics    */
    int             owner;       /* registry index of owner, -1 if none
                                    (guarded by detector->meta_mutex)       */
    int             initialized;
};

/* Detector lifecycle */
int  dg_init(deadlock_detector_t *detector);
int  dg_destroy(deadlock_detector_t *detector);
void dg_set_verbose(deadlock_detector_t *detector, int verbose);
unsigned long dg_deadlock_count(deadlock_detector_t *detector);

/* Optional explicit registry control (registration is otherwise lazy).
 * dg_thread_unregister() frees the slot; returns EBUSY if the thread still
 * owns locks or is waiting. dg_thread_register() returns the dense index
 * (>= 0) or a negative errno value. */
int  dg_thread_register(deadlock_detector_t *detector);
int  dg_thread_unregister(deadlock_detector_t *detector);

/* Mutex wrappers */
int  dg_mutex_init(dg_mutex_t *mutex, int id);
int  dg_mutex_destroy(dg_mutex_t *mutex);
int  dg_mutex_lock(dg_mutex_t *mutex, deadlock_detector_t *detector);
int  dg_mutex_unlock(dg_mutex_t *mutex, deadlock_detector_t *detector);

/* Print the current WFG as an ASCII matrix on stdout. */
void dg_dump_wfg(deadlock_detector_t *detector);

#ifdef __cplusplus
}
#endif
#endif /* DEADLOCK_GUARD_H */
