/*
 * main.c - stress test: bank transfers with circular lock acquisition.
 *
 * Worker w moves money from account w to account (w+1) % N, locking the
 * source first and the destination second. With N workers on N accounts
 * this is exactly the Dining Philosophers ring: a circular wait is
 * practically guaranteed.
 *
 *   --naive    plain pthread_mutex_lock  -> freezes in a classic deadlock
 *   --guarded  dg_mutex_lock             -> EDEADLK, release, randomized
 *                                           exponential back-off, retry
 */
#define _POSIX_C_SOURCE 200809L

#include "deadlock_guard.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_BALANCE 1000L
#define BACKOFF_BASE_US 100L
#define BACKOFF_MAX_SHIFT 8

typedef struct {
    dg_mutex_t      dg;       /* used in --guarded mode */
    pthread_mutex_t naive;    /* used in --naive mode   */
    long            balance;
} account_t;

typedef struct {
    int           guarded;
    int           threads;
    int           ops;
    long          hold_us;
    int           timeout_s;
    int           dump;
    int           quiet;
} config_t;

typedef struct {
    int           id;
    unsigned int  seed;
    unsigned long completed;
    unsigned long deadlocks_hit;   /* EDEADLK returns seen by this worker */
} worker_t;

static config_t            cfg;
static account_t          *accounts;
static deadlock_detector_t detector;
static atomic_ulong        progress;
static atomic_int          finished;

/* ---------------------------------------------------------------- */

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void die(const char *what, int rc)
{
    fprintf(stderr, "fatal: %s: %s\n", what, strerror(rc));
    exit(EXIT_FAILURE);
}

static int pick_destination(worker_t *w, int from)
{
    int n = cfg.threads;
    if (rand_r(&w->seed) % 4 == 0) {               /* 25%: random partner */
        int to = (int)(rand_r(&w->seed) % (unsigned)n);
        if (to != from)
            return to;
    }
    return (from + 1) % n;                         /* ring neighbour      */
}

/* ---------------------------------------------------------------- */
/* Naive transfer: no protection against circular wait               */
/* ---------------------------------------------------------------- */
static void transfer_naive(int from, int to, long amount)
{
    pthread_mutex_lock(&accounts[from].naive);
    if (cfg.hold_us > 0)
        sleep_us(cfg.hold_us);          /* widen the race window */
    pthread_mutex_lock(&accounts[to].naive);

    accounts[from].balance -= amount;
    accounts[to].balance   += amount;

    pthread_mutex_unlock(&accounts[to].naive);
    pthread_mutex_unlock(&accounts[from].naive);
}

/* ---------------------------------------------------------------- */
/* Guarded transfer: detect, release, back off, retry                */
/* ---------------------------------------------------------------- */
static void transfer_guarded(worker_t *w, int from, int to, long amount)
{
    int attempt = 0;

    for (;;) {
        int rc = dg_mutex_lock(&accounts[from].dg, &detector);
        if (rc != 0)
            die("dg_mutex_lock(first)", rc);   /* holding nothing: cannot cycle */

        if (cfg.hold_us > 0)
            sleep_us(cfg.hold_us);

        rc = dg_mutex_lock(&accounts[to].dg, &detector);
        if (rc == 0)
            break;                              /* got both locks */
        if (rc != EDEADLK)
            die("dg_mutex_lock(second)", rc);

        /* Cooperative resolution: give up what we hold ... */
        rc = dg_mutex_unlock(&accounts[from].dg, &detector);
        if (rc != 0)
            die("dg_mutex_unlock", rc);
        w->deadlocks_hit++;

        /* ... and sleep a randomized, exponentially growing interval. */
        int shift = attempt < BACKOFF_MAX_SHIFT ? attempt : BACKOFF_MAX_SHIFT;
        long cap  = BACKOFF_BASE_US << shift;
        sleep_us(1 + (long)(rand_r(&w->seed) % (unsigned)cap));
        attempt++;
    }

    accounts[from].balance -= amount;
    accounts[to].balance   += amount;

    if (dg_mutex_unlock(&accounts[to].dg, &detector) != 0 ||
        dg_mutex_unlock(&accounts[from].dg, &detector) != 0)
        die("dg_mutex_unlock", EPERM);
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;

    if (cfg.guarded) {
        int idx = dg_thread_register(&detector);
        if (idx < 0)
            die("dg_thread_register", -idx);
    }

    for (int i = 0; i < cfg.ops; i++) {
        int  from   = w->id;
        int  to     = pick_destination(w, from);
        long amount = 1 + (long)(rand_r(&w->seed) % 50u);

        if (cfg.guarded)
            transfer_guarded(w, from, to, amount);
        else
            transfer_naive(from, to, amount);

        w->completed++;
        atomic_fetch_add(&progress, 1);
    }

    if (cfg.guarded)
        dg_thread_unregister(&detector);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

/* ---------------------------------------------------------------- */

static void usage(const char *prog)
{
    printf("Usage: %s [--naive | --guarded] [options]\n"
           "  --naive          plain pthread mutexes (expected to deadlock)\n"
           "  --guarded        deadlock_guard wrappers (default)\n"
           "  --threads N      workers == accounts, 2..%d (default 5)\n"
           "  --ops N          transfers per worker (default 25)\n"
           "  --hold-us US     pause between 1st and 2nd lock (default 200)\n"
           "  --timeout SEC    watchdog: give up after SEC seconds without\n"
           "                   progress, 0 = hang forever (default 5)\n"
           "  --dump           print the Wait-For Graph every 250 ms (guarded)\n"
           "  --quiet          suppress per-deadlock alerts on stderr\n"
           "  --help\n", prog, DG_MAX_THREADS);
}

static int parse_long(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || end == s || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv)
{
    long v;
    cfg = (config_t){ .guarded = 1, .threads = 5, .ops = 25,
                      .hold_us = 200, .timeout_s = 5 };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--naive"))        cfg.guarded = 0;
        else if (!strcmp(a, "--guarded")) cfg.guarded = 1;
        else if (!strcmp(a, "--dump"))    cfg.dump = 1;
        else if (!strcmp(a, "--quiet"))   cfg.quiet = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); exit(0); }
        else if (!strcmp(a, "--threads") && i + 1 < argc) {
            if (parse_long(argv[++i], 2, DG_MAX_THREADS - 1, &v)) return -1;
            cfg.threads = (int)v;
        } else if (!strcmp(a, "--ops") && i + 1 < argc) {
            if (parse_long(argv[++i], 1, 100000000L, &v)) return -1;
            cfg.ops = (int)v;
        } else if (!strcmp(a, "--hold-us") && i + 1 < argc) {
            if (parse_long(argv[++i], 0, 1000000L, &v)) return -1;
            cfg.hold_us = v;
        } else if (!strcmp(a, "--timeout") && i + 1 < argc) {
            if (parse_long(argv[++i], 0, 86400L, &v)) return -1;
            cfg.timeout_s = (int)v;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", a);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0) {
        usage(argv[0]);
        return 64;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    const int n = cfg.threads;
    accounts = calloc((size_t)n, sizeof *accounts);
    worker_t  *workers = calloc((size_t)n, sizeof *workers);
    pthread_t *tids    = calloc((size_t)n, sizeof *tids);
    if (!accounts || !workers || !tids)
        die("calloc", ENOMEM);

    if (cfg.guarded) {
        int rc = dg_init(&detector);
        if (rc) die("dg_init", rc);
        dg_set_verbose(&detector, !cfg.quiet);
    }
    for (int i = 0; i < n; i++) {
        accounts[i].balance = INITIAL_BALANCE;
        int rc = cfg.guarded ? dg_mutex_init(&accounts[i].dg, i)
                             : pthread_mutex_init(&accounts[i].naive, NULL);
        if (rc) die("mutex init", rc);
    }

    printf("mode=%s threads=%d accounts=%d ops/thread=%d hold=%ldus timeout=%ds\n",
           cfg.guarded ? "GUARDED" : "NAIVE", n, n, cfg.ops, cfg.hold_us,
           cfg.timeout_s);

    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        workers[i].id   = i;
        workers[i].seed = 0x9e3779b9u * (unsigned)(i + 1);
        int rc = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
        if (rc) die("pthread_create", rc);
    }

    /* Watchdog / monitor loop (main thread). */
    unsigned long last_progress = 0;
    double        last_change   = now_s();
    while (atomic_load(&finished) < n) {
        sleep_us(cfg.dump ? 250000 : 100000);

        if (cfg.dump && cfg.guarded)
            dg_dump_wfg(&detector);

        unsigned long p = atomic_load(&progress);
        if (p != last_progress) {
            last_progress = p;
            last_change   = now_s();
        } else if (cfg.timeout_s > 0 && now_s() - last_change >= cfg.timeout_s) {
            printf("\n[WATCHDOG] no progress for %d s: %d/%d transfers done, "
                   "%d/%d workers finished.\n",
                   cfg.timeout_s, (int)p, n * cfg.ops, atomic_load(&finished), n);
            printf("[WATCHDOG] %s\n", cfg.guarded
                   ? "UNEXPECTED: guarded mode stalled."
                   : "FREEZE CONFIRMED: classic circular-wait deadlock "
                     "(every worker holds one account and waits for the next).");
            fflush(stdout);
            return cfg.guarded ? 3 : 2;      /* threads are stuck: do not join */
        }
    }

    for (int i = 0; i < n; i++)
        pthread_join(tids[i], NULL);
    double elapsed = now_s() - t0;

    unsigned long done = 0, hits = 0;
    long total = 0;
    for (int i = 0; i < n; i++) {
        done  += workers[i].completed;
        hits  += workers[i].deadlocks_hit;
        total += accounts[i].balance;
    }
    unsigned long expected = (unsigned long)n * (unsigned long)cfg.ops;

    printf("\n---- RESULT ----\n");
    printf("transfers completed : %lu / %lu (%.1f%%)\n", done, expected,
           100.0 * (double)done / (double)expected);
    printf("elapsed             : %.3f s\n", elapsed);
    printf("balance invariant   : %ld (expected %ld) -> %s\n", total,
           INITIAL_BALANCE * n, total == INITIAL_BALANCE * n ? "OK" : "VIOLATED");
    if (cfg.guarded) {
        printf("deadlocks detected  : %lu (EDEADLK seen by workers: %lu)\n",
               dg_deadlock_count(&detector), hits);
        dg_dump_wfg(&detector);
        for (int i = 0; i < n; i++)
            dg_mutex_destroy(&accounts[i].dg);
        dg_destroy(&detector);
    }

    int ok = (done == expected) && (total == INITIAL_BALANCE * n);
    printf("verdict             : %s\n", ok ? "PASS" : "FAIL");
    free(tids);
    free(workers);
    free(accounts);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
