#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Allocator Allocator;

typedef Allocator* (*allocator_create_fn)(void *memory, size_t size);
typedef void (*allocator_destroy_fn)(Allocator *a);
typedef void* (*allocator_alloc_fn)(Allocator *a, size_t size);
typedef void (*allocator_free_fn)(Allocator *a, void *ptr);

typedef struct {
    allocator_create_fn create;
    allocator_destroy_fn destroy;
    allocator_alloc_fn alloc;
    allocator_free_fn free;
} allocator_api_t;
