#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../include/allocator_api.h"

static inline uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec; }
static size_t page_size(void){ static size_t p=0; if(!p) p=(size_t)sysconf(_SC_PAGESIZE); return p; }
static size_t align_up_size(size_t x, size_t a){ return (x + (a-1)) & ~(a-1); }

// -------- Fallback wrapper on top of mmap() --------
typedef struct { int dummy; } Fallback;

static Allocator* fb_create(void *mem, size_t sz){ (void)mem; (void)sz; return (Allocator*)calloc(1, sizeof(Fallback)); }
static void fb_destroy(Allocator *a){ free(a); }
static void* fb_alloc(Allocator *a, size_t size){ (void)a; if(size==0) return NULL; size_t hdr = sizeof(size_t);
    size_t total = align_up_size(size + hdr, page_size());
    void *p = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if(p==MAP_FAILED) return NULL;
    *(size_t*)p = total; // store mapping size at start
    return (unsigned char*)p + hdr;
}
static void fb_free(Allocator *a, void *ptr){ (void)a; if(!ptr) return; unsigned char *base = (unsigned char*)ptr - sizeof(size_t); size_t total = *(size_t*)base; munmap(base, total); }

int main(int argc, char **argv){
    const char *libpath = argc>1 ? argv[1] : NULL;
    size_t arena = (argc>2) ? strtoull(argv[2], NULL, 10) : (1ull<<20); // 1 MiB default

    allocator_api_t api = {0};
    void *handle = NULL;
    if (libpath) {
        handle = dlopen(libpath, RTLD_NOW);
        if (handle) {
            api.create = (allocator_create_fn)dlsym(handle, "allocator_create");
            api.destroy = (allocator_destroy_fn)dlsym(handle, "allocator_destroy");
            api.alloc   = (allocator_alloc_fn)dlsym(handle, "allocator_alloc");
            api.free    = (allocator_free_fn)dlsym(handle, "allocator_free");
        }
    }
    if (!api.create || !api.destroy || !api.alloc || !api.free) {
        fprintf(stderr, "Using fallback mmap-based allocator (dlopen failed or missing symbols)\n");
        api.create = fb_create; api.destroy = fb_destroy; api.alloc = fb_alloc; api.free = fb_free;
    }

    // Allocate arena via mmap and init allocator (fallback ignores it)
    void *memory = NULL; size_t psz = page_size(); size_t asz = align_up_size(arena, psz);
    if (api.create != fb_create) {
        memory = mmap(NULL, asz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) { perror("mmap arena"); return 1; }
    }

    Allocator *A = api.create(memory, asz);
    if (!A) { fprintf(stderr, "allocator_create failed\n"); if(memory) munmap(memory, asz); return 1; }

    // Simple benchmark: many alloc/free with random sizes
    const size_t N = (argc>3) ? strtoull(argv[3], NULL, 10) : 100000;
    const size_t min_sz = 8, max_sz = 4096;
    void **ptrs = (void**)malloc(N * sizeof(void*));
    size_t *sizes = (size_t*)malloc(N * sizeof(size_t));
    if (!ptrs || !sizes) { fprintf(stderr, "oom\n"); return 1; }

    unsigned seed = 12345;
    for (size_t i=0;i<N;i++){ sizes[i]= (size_t)(min_sz + (rand_r(&seed) % (max_sz-min_sz+1))); }

    uint64_t t0 = now_ns();
    for (size_t i=0;i<N;i++){ ptrs[i]= api.alloc(A, sizes[i]); if(!ptrs[i]){ fprintf(stderr, "alloc failed at %zu\n", i); break; } }
    uint64_t t1 = now_ns();
    for (size_t i=0;i<N;i++){ api.free(A, ptrs[i]); }
    uint64_t t2 = now_ns();

    double alloc_ms = (t1-t0)/1e6, free_ms = (t2-t1)/1e6;
    printf("allocs=%zu alloc_ms=%.3f free_ms=%.3f per_alloc_ns=%.1f per_free_ns=%.1f\n",
           N, alloc_ms, free_ms, (t1-t0)/(double)N, (t2-t1)/(double)N);

    free(ptrs); free(sizes);
    api.destroy(A);
    if (api.create != fb_create) munmap(memory, asz);
    if (handle) dlclose(handle);
    return 0;
}
