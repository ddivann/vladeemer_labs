#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>


typedef struct Allocator {
    unsigned char *base;
    size_t size;
    struct block_header * free_list;
} Allocator;

typedef struct block_header {
    size_t size;
    struct block_header *next;
    int free;
} block_header;

static inline size_t align_up(size_t x, size_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

void* allocator_alloc(Allocator *a, size_t size);
void allocator_free(Allocator *a, void *ptr);
Allocator* allocator_create(void *memory, size_t size);
void allocator_destroy(Allocator *a);
