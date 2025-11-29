#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#ifdef MODE_ATOMIC
#include <stdatomic.h>
#include <sched.h>
#endif

#ifndef MODE_SYNC
#ifndef MODE_ATOMIC
#error "Define MODE_SYNC or MODE_ATOMIC via compiler flags"
#endif
#endif

// -------------------- Utils --------------------
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t next_pow2(size_t n) {
    if (n < 2) return 1;
    n--;
    for (size_t i = 1; i < sizeof(size_t) * 8; i <<= 1) n |= n >> i;
    return n + 1;
}

static void print_thread_count() {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { perror("fopen /proc/self/status"); return; }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        if (strncmp(line, "Threads:", 8) == 0) {
            fputs(line, stdout);
            break;
        }
    }
    free(line);
    fclose(f);
}

// -------------------- Barriers --------------------
#ifdef MODE_SYNC

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int count;
    int trip;
    int n;
} barrier_t;

static int barrier_init(barrier_t *b, int n) {
    b->count = 0; b->trip = 0; b->n = n;
    if (pthread_mutex_init(&b->m, NULL)) return -1;
    if (pthread_cond_init(&b->cv, NULL)) return -1;
    return 0;
}

static void barrier_destroy(barrier_t *b) {
    pthread_mutex_destroy(&b->m);
    pthread_cond_destroy(&b->cv);
}

static void barrier_wait(barrier_t *b) {
    pthread_mutex_lock(&b->m);
    int trip = b->trip;
    if (++b->count == b->n) {
        b->trip++;
        b->count = 0;
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->m);
        return;
    }
    while (trip == b->trip) {
        pthread_cond_wait(&b->cv, &b->m);
    }
    pthread_mutex_unlock(&b->m);
}

#else // MODE_ATOMIC

typedef struct {
    atomic_int count;
    atomic_int sense;
    int n;
} barrier_t;

static int barrier_init(barrier_t *b, int n) {
    atomic_store(&b->count, 0);
    atomic_store(&b->sense, 0);
    b->n = n;
    return 0;
}

static void barrier_wait_local(barrier_t *b, int *local_sense) {
    int ls = *local_sense;
    if (atomic_fetch_add_explicit(&b->count, 1, memory_order_acq_rel) == b->n - 1) {
        atomic_store_explicit(&b->count, 0, memory_order_release);
        atomic_store_explicit(&b->sense, !ls, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->sense, memory_order_acquire) == ls) {
            sched_yield();
        }
    }
    *local_sense = !ls;
}

#endif

// -------------------- Sort context --------------------
typedef struct {
    int *a;
    size_t n;         // original size
    size_t np2;       // padded size (power of two)
    int tid;
    int nthreads;
#ifdef MODE_ATOMIC
    barrier_t *stage_barrier;
    // atomic start flag for starting work
    _Atomic bool *start_flag;
#else
    barrier_t *start_barrier;
    barrier_t *stage_barrier;
#endif
} worker_ctx_t;

// -------------------- Bitonic worker --------------------
static inline void compare_swap(int *a, size_t i, size_t l, int ascend) {
    int ai = a[i];
    int al = a[l];
    if ((ai > al) == ascend) {
        a[i] = al;
        a[l] = ai;
    }
}

static void *worker_fn(void *arg) {
    worker_ctx_t *w = (worker_ctx_t *)arg;
#ifdef MODE_ATOMIC
    int local_sense = 0;
    // wait for start
    while (!atomic_load_explicit(w->start_flag, memory_order_acquire)) {
        sched_yield();
    }
#else
    // sync start with main for optional pause/demo
    barrier_wait(w->start_barrier);
#endif

    size_t np2 = w->np2;
    size_t chunk = np2 / (size_t)w->nthreads;
    size_t start = (size_t)w->tid * chunk;
    size_t end = (w->tid == w->nthreads - 1) ? np2 : start + chunk;
    if (end > np2) end = np2;

    for (size_t k = 2; k <= np2; k <<= 1) {
        for (size_t j = k >> 1; j > 0; j >>= 1) {
            // process range
            for (size_t i = start; i < end; i++) {
                size_t l = i ^ j;
                if (l > i) {
                    int ascend = ((i & k) == 0);
                    compare_swap(w->a, i, l, ascend);
                }
            }
            // barrier between stages
#ifdef MODE_ATOMIC
            barrier_wait_local(w->stage_barrier, &local_sense);
#else
            barrier_wait(w->stage_barrier);
#endif
        }
    }
    return NULL;
}

// -------------------- CLI --------------------
static void usage(const char *prog) {
#ifdef MODE_SYNC
    const char *mode = "sync";
#else
    const char *mode = "atomic";
#endif
    fprintf(stderr,
        "Usage: %s -n <size> -t <threads> [-c] [--seed N] [--pause S] [--print-threads]\n"
        "  mode: %s\n"
        "  -n <size>         number of elements (will be padded to power of two)\n"
        "  -t <threads>      number of worker threads (>=1)\n"
        "  -c                verify sort\n"
        "  --seed N          RNG seed (default: time)\n"
        "  --pause S         sleep S seconds after threads are created, before start\n"
        "  --print-threads   print current Threads: count from /proc/self/status\n",
        prog, mode);
}

int main(int argc, char **argv) {
    size_t n = 0;
    int nthreads = 0;
    int verify = 0;
    int pause_sec = 0;
    unsigned seed = (unsigned)time(NULL);
    int print_threads = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            n = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            nthreads = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-c")) {
            verify = 1;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--pause") && i + 1 < argc) {
            pause_sec = (int)strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--print-threads")) {
            print_threads = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (n == 0 || nthreads <= 0) {
        usage(argv[0]);
        return 1;
    }

    size_t np2 = next_pow2(n);
    int *a = (int *)malloc(np2 * sizeof(int));
    if (!a) { perror("malloc"); return 1; }

    srand(seed);
    for (size_t i = 0; i < n; i++) a[i] = (int)rand();
    for (size_t i = n; i < np2; i++) a[i] = INT32_MAX; // padding

    if (nthreads > (int)np2) nthreads = (int)np2; // cap threads
    if (nthreads < 1) nthreads = 1;

    pthread_t *ths = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    worker_ctx_t *ctxs = (worker_ctx_t *)calloc((size_t)nthreads, sizeof(worker_ctx_t));
    if (!ths || !ctxs) { perror("calloc"); return 1; }

#ifdef MODE_ATOMIC
    barrier_t stage_barrier;
    barrier_init(&stage_barrier, nthreads);
    _Atomic bool start_flag = ATOMIC_VAR_INIT(false);
#else
    barrier_t start_barrier;
    barrier_t stage_barrier;
    barrier_init(&start_barrier, nthreads + 1);
    barrier_init(&stage_barrier, nthreads);
#endif

    for (int t = 0; t < nthreads; ++t) {
        ctxs[t].a = a;
        ctxs[t].n = n;
        ctxs[t].np2 = np2;
        ctxs[t].tid = t;
        ctxs[t].nthreads = nthreads;
#ifdef MODE_ATOMIC
        ctxs[t].stage_barrier = &stage_barrier;
        ctxs[t].start_flag = &start_flag;
#else
        ctxs[t].start_barrier = &start_barrier;
        ctxs[t].stage_barrier = &stage_barrier;
#endif
        if (pthread_create(&ths[t], NULL, worker_fn, &ctxs[t])) {
            perror("pthread_create");
            return 1;
        }
    }

    if (print_threads) {
        print_thread_count();
    }
    if (pause_sec > 0) {
        fprintf(stderr, "Pausing %d s before sort start...\n", pause_sec);
        sleep((unsigned)pause_sec);
    }

    uint64_t t0 = now_ns();
#ifdef MODE_ATOMIC
    atomic_store_explicit(&start_flag, true, memory_order_release);
#else
    barrier_wait(&start_barrier); // release workers to start
#endif

    for (int t = 0; t < nthreads; ++t) pthread_join(ths[t], NULL);
    uint64_t t1 = now_ns();

    double ms = (double)(t1 - t0) / 1.0e6;
    printf("Time: %.3f ms, n=%zu, threads=%d\n", ms, n, nthreads);

    if (verify) {
        bool ok = true;
        for (size_t i = 1; i < n; i++) if (a[i-1] > a[i]) { ok = false; break; }
        printf("Verify: %s\n", ok ? "OK" : "FAIL");
    }

#ifndef MODE_ATOMIC
    barrier_destroy(&start_barrier);
#endif
#ifndef MODE_ATOMIC
    barrier_destroy(&stage_barrier);
#else
    (void)stage_barrier; // nothing to destroy
#endif

    free(ths);
    free(ctxs);
    free(a);
    return 0;
}
