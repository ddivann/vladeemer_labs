#pragma once
#include <stddef.h>
#include <stdint.h>

// Buddy allocator (McKusick–Karels style size classes)

typedef struct Allocator {
    unsigned char *base;
    size_t size;
    int min_order;   // minimal block = 1<<min_order
    int max_order;   // maximal block = 1<<max_order (fits into size)
    struct FreeNode *free_lists[64];
} Allocator;

void* allocator_alloc(Allocator *a, size_t size);
void allocator_free(Allocator *a, void *ptr);
Allocator* allocator_create(void *memory, size_t size);
void allocator_destroy(Allocator *a);
