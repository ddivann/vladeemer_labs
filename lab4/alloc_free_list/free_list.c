#include "free_list.h"
#include <string.h>

Allocator* allocator_create(void * memory, size_t size) {
    Allocator *a = (Allocator*)memory;
    a->base = (unsigned char*)memory;
    a->size = size;

    size_t off = align_up(sizeof(Allocator), alignof(block_header));
    block_header *b = (block_header*)(a->base + off);
    size_t usable = size - off;
    b->size = usable;
    b->next = NULL;
    b->free = 1;
    a->free_list = b;
    return a;
}

void allocator_destroy(Allocator *a) {
    (void)a;
}

static void split_block(block_header *b, size_t need) {

    if (b->size >= need + sizeof(block_header) + 16) {
        block_header *nb = (block_header*)((unsigned char*)b + need);
        nb->size = b->size - need;
        nb->next = b->next;
        nb->free = 1;
        b->size = need;
        b->next = nb;
    }
}

void* allocator_alloc(Allocator *a, size_t size) {
    if (size == 0) return NULL;
    size_t need = align_up(size, alignof(max_align_t)) + sizeof(block_header);
    block_header **pp = &a->free_list;
    block_header *p = a->free_list;
    while (p) {
        if (p->free && p->size >= need) {
            split_block(p, need);
            p->free = 0;
            // Remove from free list
            *pp = p->next;
            p->next = NULL;
            return (unsigned char*)p + sizeof(block_header);
        }
        pp = &p->next;
        p = p->next;
    }
    return NULL;
}

static block_header* ptr_to_block(void *ptr) {
    return (block_header*)((unsigned char*)ptr - sizeof(block_header));
}

void allocator_free(Allocator *a, void *ptr) {
    if (!ptr) return;
    block_header *b = ptr_to_block(ptr);
    b->free = 1;
    
    // Insert in sorted order by address for better coalescing
    block_header **pp = &a->free_list;
    block_header *p = a->free_list;
    while (p && p < b) {
        pp = &p->next;
        p = p->next;
    }
    b->next = p;
    *pp = b;
    
    // Try to coalesce with next block
    if (b->next && b->next->free) {
        unsigned char *end = (unsigned char*)b + b->size;
        if (end == (unsigned char*)b->next) {
            b->size += b->next->size;
            b->next = b->next->next;
        }
    }
    
    // Try to coalesce with previous block
    pp = &a->free_list;
    p = a->free_list;
    while (p && p != b) {
        if (p->next == b) {
            unsigned char *end = (unsigned char*)p + p->size;
            if (end == (unsigned char*)b) {
                p->size += b->size;
                p->next = b->next;
                break;
            }
        }
        pp = &p->next;
        p = p->next;
    }
}
