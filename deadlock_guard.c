/*
 * deadlock_guard.c - implementation of the runtime deadlock detector.
 * See deadlock_guard.h for the model and the lock hierarchy.
 */
#define _POSIX_C_SOURCE 200809L

#include "deadlock_guard.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers - all *_locked functions require meta_mutex held.  */
/* ------------------------------------------------------------------ */

static void wfg_clear_row(deadlock_detector_t *d, int t)
{
    memset(d->wfg[t], 0, sizeof d->wfg[t]);
}

static void wfg_clear_col(deadlock_detector_t *d, int t)
{
    for (int i = 0; i < DG_MAX_THREADS; i++)
        d->wfg[i][t] = 0;
}

static int registry_find_locked(const deadlock_detector_t *d, pthread_t self)
{
    for (int i = 0; i < DG_MAX_THREADS; i++)
        if (d->threads[i].in_use && pthread_equal(d->threads[i].tid, self))
            return i;
    return -1;
}

static int registry_get_or_add_locked(deadlock_detector_t *d, pthread_t self)
{
    int i = registry_find_locked(d, self);
    if (i >= 0)
        return i;
    for (i = 0; i < DG_MAX_THREADS; i++) {
        if (!d->threads[i].in_use) {
            d->threads[i].in_use     = 1;
            d->threads[i].tid        = self;
            d->threads[i].held_count = 0;
            d->threads[i].waiting_on = NULL;
            wfg_clear_row(d, i);
            wfg_clear_col(d, i);
            d->thread_count++;
            return i;
        }
    }
    return -1;
}

/*
 * Re-derive the outgoing edge of every thread blocked on `m` from the
 * mutex's *current* owner. Called whenever ownership of `m` changes, so a
 * waiter's edge never points at a stale holder.
 */
static void refresh_waiters_locked(deadlock_detector_t *d, const dg_mutex_t *m)
{
    for (int t = 0; t < DG_MAX_THREADS; t++) {
        if (d->threads[t].in_use && d->threads[t].waiting_on == m) {
            wfg_clear_row(d, t);
            if (m->owner >= 0 && m->owner != t)
                d->wfg[t][m->owner] = 1;
        }
    }
}

/*
 * DFS from u looking for a path back to `target`. On success path[] holds
 * start ... target (the closing node is appended) and *len is its length.
 */
static int dfs_cycle_locked(const deadlock_detector_t *d, int u, int target,
                            unsigned char *visited, int *path, int *len)
{
    visited[u] = 1;
    for (int v = 0; v < DG_MAX_THREADS; v++) {
        if (!d->wfg[u][v])
            continue;
        if (v == target) {
            path[(*len)++] = v;
            return 1;
        }
        if (!visited[v]) {
            path[(*len)++] = v;
            if (dfs_cycle_locked(d, v, target, visited, path, len))
                return 1;
            (*len)--;
        }
    }
    return 0;
}

static int find_cycle_locked(const deadlock_detector_t *d, int start,
                             int *path, int *len)
{
    unsigned char visited[DG_MAX_THREADS];
    memset(visited, 0, sizeof visited);
    path[0] = start;
    *len = 1;
    return dfs_cycle_locked(d, start, start, visited, path, len);
}

static void report_cycle_locked(const deadlock_detector_t *d,
                                const int *path, int len)
{
    char line[512];
    size_t off = 0;

    off += (size_t)snprintf(line + off, sizeof line - off,
                            "[DEADLOCK DETECTED] Cycle: ");
    for (int i = 0; i < len && off < sizeof line; i++)
        off += (size_t)snprintf(line + off, sizeof line - off, "%sT%d",
                                i ? " -> " : "", path[i]);

    fprintf(stderr, "%s\n", line);
    for (int i = 0; i + 1 < len; i++) {
        const dg_mutex_t *w = d->threads[path[i]].waiting_on;
        fprintf(stderr, "    T%d waits for M%d (held by T%d)\n",
                path[i], w ? w->id : -1, path[i + 1]);
    }
    fprintf(stderr, "    -> acquisition refused with EDEADLK; caller must back off\n");
}

/* ------------------------------------------------------------------ */
/* Detector lifecycle                                                  */
/* ------------------------------------------------------------------ */

int dg_init(deadlock_detector_t *d)
{
    if (!d)
        return EINVAL;
    memset(d, 0, sizeof *d);
    int rc = pthread_mutex_init(&d->meta_mutex, NULL);
    if (rc != 0)
        return rc;
    d->verbose     = 1;
    d->initialized = 1;
    return 0;
}

int dg_destroy(deadlock_detector_t *d)
{
    if (!d || !d->initialized)
        return EINVAL;

    pthread_mutex_lock(&d->meta_mutex);
    for (int i = 0; i < DG_MAX_THREADS; i++) {
        if (d->threads[i].in_use && d->threads[i].waiting_on) {
            pthread_mutex_unlock(&d->meta_mutex);
            return EBUSY;
        }
    }
    d->initialized = 0;
    pthread_mutex_unlock(&d->meta_mutex);
    return pthread_mutex_destroy(&d->meta_mutex);
}

void dg_set_verbose(deadlock_detector_t *d, int verbose)
{
    if (!d || !d->initialized)
        return;
    pthread_mutex_lock(&d->meta_mutex);
    d->verbose = verbose;
    pthread_mutex_unlock(&d->meta_mutex);
}

unsigned long dg_deadlock_count(deadlock_detector_t *d)
{
    unsigned long n;
    if (!d || !d->initialized)
        return 0;
    pthread_mutex_lock(&d->meta_mutex);
    n = d->deadlocks_detected;
    pthread_mutex_unlock(&d->meta_mutex);
    return n;
}

int dg_thread_register(deadlock_detector_t *d)
{
    int idx;
    if (!d || !d->initialized)
        return -EINVAL;
    pthread_mutex_lock(&d->meta_mutex);
    idx = registry_get_or_add_locked(d, pthread_self());
    pthread_mutex_unlock(&d->meta_mutex);
    return idx >= 0 ? idx : -EAGAIN;
}

int dg_thread_unregister(deadlock_detector_t *d)
{
    int rc = 0;
    if (!d || !d->initialized)
        return EINVAL;
    pthread_mutex_lock(&d->meta_mutex);
    int idx = registry_find_locked(d, pthread_self());
    if (idx >= 0) {
        if (d->threads[idx].held_count > 0 || d->threads[idx].waiting_on) {
            rc = EBUSY;
        } else {
            wfg_clear_row(d, idx);
            wfg_clear_col(d, idx);
            d->threads[idx].in_use = 0;
            d->thread_count--;
        }
    }
    pthread_mutex_unlock(&d->meta_mutex);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Mutex wrappers                                                      */
/* ------------------------------------------------------------------ */

int dg_mutex_init(dg_mutex_t *m, int id)
{
    if (!m)
        return EINVAL;
    int rc = pthread_mutex_init(&m->real, NULL);
    if (rc != 0)
        return rc;
    m->id          = id;
    m->owner       = -1;
    m->initialized = 1;
    return 0;
}

int dg_mutex_destroy(dg_mutex_t *m)
{
    if (!m || !m->initialized)
        return EINVAL;
    if (m->owner >= 0)
        return EBUSY;
    m->initialized = 0;
    return pthread_mutex_destroy(&m->real);
}

int dg_mutex_lock(dg_mutex_t *m, deadlock_detector_t *d)
{
    int rc;
    int path[DG_MAX_THREADS + 1];
    int len;

    if (!m || !d || !m->initialized || !d->initialized)
        return EINVAL;

    /* ---- Phase 1: speculative edge + cycle check (meta held) ------- */
    pthread_mutex_lock(&d->meta_mutex);

    int me = registry_get_or_add_locked(d, pthread_self());
    if (me < 0) {
        pthread_mutex_unlock(&d->meta_mutex);
        return EAGAIN;
    }

    if (m->owner == me) {
        /* Self-deadlock: trivial cycle T -> T on a non-recursive mutex. */
        d->deadlocks_detected++;
        if (d->verbose) {
            fprintf(stderr, "[DEADLOCK DETECTED] Cycle: T%d -> T%d\n"
                            "    T%d re-locks M%d which it already owns\n"
                            "    -> acquisition refused with EDEADLK\n",
                    me, me, me, m->id);
        }
        pthread_mutex_unlock(&d->meta_mutex);
        return EDEADLK;
    }

    d->threads[me].waiting_on = m;

    if (m->owner >= 0) {
        d->wfg[me][m->owner] = 1;                       /* speculative edge */

        if (find_cycle_locked(d, me, path, &len)) {
            d->wfg[me][m->owner] = 0;                   /* roll back        */
            d->deadlocks_detected++;
            if (d->verbose)
                report_cycle_locked(d, path, len);
            d->threads[me].waiting_on = NULL;
            pthread_mutex_unlock(&d->meta_mutex);
            return EDEADLK;
        }
    }
    /* No cycle: edge (if any) stays. Drop meta BEFORE blocking (Rule 1). */
    pthread_mutex_unlock(&d->meta_mutex);

    /* ---- Phase 2: block on the real mutex (meta NOT held) ---------- */
    rc = pthread_mutex_lock(&m->real);

    /* ---- Phase 3: publish ownership, clear waiting edges ----------- */
    pthread_mutex_lock(&d->meta_mutex);
    d->threads[me].waiting_on = NULL;
    wfg_clear_row(d, me);
    if (rc == 0) {
        m->owner = me;
        d->threads[me].held_count++;
        /* Remaining waiters of m now wait for us, not for the old owner. */
        refresh_waiters_locked(d, m);
    }
    pthread_mutex_unlock(&d->meta_mutex);
    return rc;
}

int dg_mutex_unlock(dg_mutex_t *m, deadlock_detector_t *d)
{
    if (!m || !d || !m->initialized || !d->initialized)
        return EINVAL;

    pthread_mutex_lock(&d->meta_mutex);
    int me = registry_find_locked(d, pthread_self());
    if (me < 0 || m->owner != me) {
        pthread_mutex_unlock(&d->meta_mutex);
        return EPERM;
    }
    /* Clear ownership BEFORE the real unlock so nobody sees a stale owner. */
    m->owner = -1;
    d->threads[me].held_count--;
    refresh_waiters_locked(d, m);       /* waiters of m lose their edge */
    pthread_mutex_unlock(&d->meta_mutex);

    return pthread_mutex_unlock(&m->real);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

void dg_dump_wfg(deadlock_detector_t *d)
{
    int idx[DG_MAX_THREADS];
    int n = 0;

    if (!d || !d->initialized)
        return;

    pthread_mutex_lock(&d->meta_mutex);

    for (int i = 0; i < DG_MAX_THREADS; i++)
        if (d->threads[i].in_use)
            idx[n++] = i;

    printf("=== Wait-For Graph (row waits-for column) | threads=%d deadlocks=%lu ===\n",
           n, d->deadlocks_detected);
    if (n == 0) {
        printf("(no registered threads)\n");
    } else {
        printf("      ");
        for (int c = 0; c < n; c++)
            printf(" T%-2d", idx[c]);
        printf("\n");
        for (int r = 0; r < n; r++) {
            printf("  T%-2d ", idx[r]);
            for (int c = 0; c < n; c++)
                printf("  %c ", d->wfg[idx[r]][idx[c]] ? 'X' : '.');
            const dg_thread_slot_t *s = &d->threads[idx[r]];
            printf("  | holds=%d", s->held_count);
            if (s->waiting_on)
                printf(" waiting=M%d", s->waiting_on->id);
            printf("\n");
        }
    }
    printf("==============================================================\n");
    fflush(stdout);

    pthread_mutex_unlock(&d->meta_mutex);
}
