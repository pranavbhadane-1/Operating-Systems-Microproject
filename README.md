# deadlock_guard - In-Process Runtime Deadlock Detection & Resolution for pthreads

A small C11/POSIX library plus a stress-test demo. It detects circular lock
waits **before** a thread blocks, refuses the acquisition with `EDEADLK`, and
lets the application resolve it cooperatively (release, back off, retry).
Only libc and libpthread are required.

## Build

    make                # produces ./deadlock_demo  (-std=c11 -Wall -Wextra -pthread -O2)
    make clean

## Usage

    ./deadlock_demo --naive              # plain pthread mutexes: freezes (watchdog reports it after 5 s)
    ./deadlock_demo --naive --timeout 0  # hang forever (use Ctrl-C)
    ./deadlock_demo --guarded            # detector on: 100% of transfers complete
    ./deadlock_demo --guarded --quiet --threads 12 --ops 500
    ./deadlock_demo --guarded --dump --hold-us 1000   # live Wait-For Graph snapshots

| Option | Meaning |
|---|---|
| `--naive` / `--guarded` | lock implementation (default `--guarded`) |
| `--threads N` | workers = accounts, 2..63 (default 5) |
| `--ops N` | transfers per worker (default 25) |
| `--hold-us US` | pause between 1st and 2nd lock; widens the race window (default 200) |
| `--timeout SEC` | watchdog: exit after SEC s without progress, `0` = never (default 5) |
| `--dump` | print the WFG matrix every 250 ms |
| `--quiet` | suppress per-deadlock alerts |

Exit codes: `0` PASS, `1` FAIL (lost work / invariant broken), `2` naive freeze
confirmed by watchdog, `3` guarded mode stalled (should never happen), `64` bad CLI.

## Scenario

Worker *w* transfers money from account *w* to account *(w+1) mod N*, locking
the source first and the destination second. With N workers on N accounts this
is the Dining-Philosophers ring: every worker grabs one lock and waits for the
next, i.e. a guaranteed circular wait. In `--guarded` mode the final check also
verifies the money-conservation invariant (sum of balances unchanged).

## Architecture

    thread  --dg_mutex_lock-->  [meta_mutex: registry + WFG + DFS]  -->  real pthread_mutex_t
                                       (never held while blocking)

* **Thread registry** - `pthread_t` -> dense index `0..DG_MAX_THREADS-1`, allocated lazily
  (or explicitly via `dg_thread_register` / `dg_thread_unregister`).
* **Wait-For Graph** - `wfg[a][b] = 1` means thread *a* waits for a mutex owned by *b*.
* **Cycle detection** - on `dg_mutex_lock`: add the speculative edge `me -> owner`,
  run a DFS looking for a path back to `me`.
  * cycle: roll the edge back, print `[DEADLOCK DETECTED] Cycle: T1 -> T2 -> T1`
    with the mutex on each edge, return `EDEADLK`; nothing blocks, nothing leaks.
  * no cycle: keep the edge, drop `meta_mutex`, block on the real mutex; afterwards
    re-take `meta_mutex`, record ownership, clear this thread's wait edges.
  * re-locking a mutex you already own is a trivial cycle `T -> T` -> `EDEADLK`.
* **Edge accuracy** - whenever ownership of a mutex changes (acquire / unlock), the
  out-edges of all threads waiting on that mutex are re-derived from the new owner,
  so edges never point at a stale holder.
* **Meta-deadlock prevention (lock hierarchy)** - `meta_mutex` is a leaf lock:
  held only for short non-blocking sections and *never* while blocking on a user
  mutex. The only nesting that occurs is user mutex -> meta, so the detector
  cannot take part in a cycle. On unlock, ownership is cleared under `meta_mutex`
  *before* the real `pthread_mutex_unlock`.
* **Resolution policy lives in the caller** - the demo releases its held lock,
  sleeps a random time in `[1, 100us << min(attempt, 8)]` (randomized exponential
  back-off, which breaks livelock symmetry) and retries.

## API summary (`deadlock_guard.h`)

    int  dg_init(deadlock_detector_t *);          int  dg_destroy(deadlock_detector_t *);
    int  dg_mutex_init(dg_mutex_t *, int id);     int  dg_mutex_destroy(dg_mutex_t *);
    int  dg_mutex_lock(dg_mutex_t *, deadlock_detector_t *);   /* 0, EDEADLK, EINVAL, EAGAIN */
    int  dg_mutex_unlock(dg_mutex_t *, deadlock_detector_t *); /* 0, EPERM, EINVAL */
    void dg_dump_wfg(deadlock_detector_t *);
    /* extras */ dg_set_verbose, dg_deadlock_count, dg_thread_register, dg_thread_unregister

## Known limits (good viva talking points)

* Only `dg_mutex_t` locks are visible; plain `pthread_mutex_t` and other
  blocking primitives (condvars, joins, I/O) are not modelled.
* Mutexes are non-recursive. One detector instance per set of interacting mutexes.
* Detection is exact for the mutexes it manages (each thread waits on at most one
  lock, so the WFG has out-degree <= 1 and the DFS is O(N) in practice, O(N^2)
  worst case on the matrix). Resolution requires cooperative callers.
* Registry is bounded by `DG_MAX_THREADS` (64); call `dg_thread_unregister` on thread exit.
* ThreadSanitizer reports *lock-order-inversion* for the demo's deliberately
  inverted acquisition order. That is the hazard being demonstrated; no data races are reported.
