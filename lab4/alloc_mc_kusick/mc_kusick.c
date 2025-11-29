#include "mc_kusick.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// Simple buddy allocator with power-of-two size classes (McKusick–Karels style)
// We store a small header (uint16_t order) at the beginning of each allocated block.

typedef struct FreeNode { struct FreeNode *next; } FreeNode;

typedef struct Allocator Allocator; // forward (from header)

static inline size_t align_up(size_t x, size_t a){ return (x + (a-1)) & ~(a-1); }
static inline size_t pow2(int k){ return (size_t)1 << k; }

static int ilog2_ceil(size_t x){
    int k = 0; size_t v = 1;
    while (v < x) { v <<= 1; k++; }
    return k;
}

Allocator* allocator_create(void *memory, size_t size){
    Allocator *A = (Allocator*)memory;
    A->base = (unsigned char*)memory;
    A->size = size;

    size_t off = align_up(sizeof(Allocator), 16);
    size_t usable = size - off;
    if (usable < 64) return NULL;

    A->min_order = 4; // 16 bytes minimal block

    if (pow2(A->min_order) < sizeof(uint16_t) + 8) A->min_order = ilog2_ceil(sizeof(uint16_t)+8);

    // Max order such that a single block fits in usable
    // Limit max_order to 20 (1MB blocks) to avoid excessive alignment padding
    A->max_order = 0;
    while (A->max_order < 20 && pow2(A->max_order+1) <= usable) A->max_order++;
    if (A->max_order < A->min_order) return NULL;

    for (int i=0;i<64;i++) A->free_lists[i] = NULL;

    // Align starting region to largest block size boundary
    unsigned char *arena = A->base + off;
    size_t mask = pow2(A->max_order) - 1;
    uintptr_t aligned = ((uintptr_t)arena + mask) & ~((uintptr_t)mask);
    size_t align_pad = (size_t)(aligned - (uintptr_t)arena);
    if (align_pad >= usable) return NULL;
    arena = (unsigned char*)aligned;
    usable -= align_pad;
    usable &= ~mask; // round down to multiple of largest block size

    // Insert all memory into free lists splitting into max_order blocks
    size_t blocks = usable >> A->max_order;
    for (size_t i=0;i<blocks;i++){
        unsigned char *b = arena + (i << A->max_order);
        FreeNode *node = (FreeNode*)b;
        node->next = (FreeNode*)A->free_lists[A->max_order];
        A->free_lists[A->max_order] = node;
    }
    return A;
}

void allocator_destroy(Allocator *a){ (void)a; }

static int order_for(size_t size, int min_order){
    // include header in returned block
    size_t need = size + sizeof(uint16_t);
    int k = min_order;
    while (pow2(k) < need) k++;
    return k;
}

static void list_push(FreeNode **head, FreeNode *n){ n->next = *head; *head = n; }
static FreeNode* list_pop(FreeNode **head){ FreeNode *n = *head; if(n) *head = n->next; return n; }
static int list_remove(FreeNode **head, FreeNode *n){
    FreeNode **pp = head; FreeNode *p = *head;
    while (p){ if (p==n){ *pp = p->next; return 1; } pp = &p->next; p = p->next; }
    return 0;
}

void* allocator_alloc(Allocator *A, size_t size){
    if (size==0) return NULL;
    int k = order_for(size, A->min_order);
    if (k > A->max_order) return NULL;
    int cur = k;
    while (cur <= A->max_order && A->free_lists[cur] == NULL) cur++;
    if (cur > A->max_order) return NULL; // out of memory
    // split down from cur to k
    FreeNode *node = list_pop((FreeNode**)&A->free_lists[cur]);
    unsigned char *block = (unsigned char*)node;
    while (cur > k){
        cur--;
        // split block into two buddies of order cur
        unsigned char *buddy = block + pow2(cur);
        list_push((FreeNode**)&A->free_lists[cur], (FreeNode*)buddy);
        // keep block as first half
    }
    // mark header
    uint16_t *hdr = (uint16_t*)block; *hdr = (uint16_t)k;
    return block + sizeof(uint16_t);
}

void allocator_free(Allocator *A, void *ptr){
    if (!ptr) return;
    unsigned char *p = (unsigned char*)ptr - sizeof(uint16_t);
    uint16_t k = *(uint16_t*)p;
    unsigned char *block = p; // header at start of block
    // Try to coalesce up
    for(;;){
        // find buddy
        uintptr_t rel = (uintptr_t)block - (uintptr_t)A->base;
        uintptr_t buddy_rel = rel ^ pow2(k);
        unsigned char *buddy = A->base + buddy_rel;
        // Is buddy within arena and free at same order?
        int found = 0;
        FreeNode *it = (FreeNode*)A->free_lists[k];
        while (it){ if ((unsigned char*)it == buddy){ found = 1; break; } it = it->next; }
        if (!found) {
            // cannot merge; push this block into list k
            list_push((FreeNode**)&A->free_lists[k], (FreeNode*)block);
            break;
        }
        // remove buddy and merge
        list_remove((FreeNode**)&A->free_lists[k], (FreeNode*)buddy);
        // new block is min(block, buddy)
        block = (buddy < block) ? buddy : block;
        k++;
        if (k > A->max_order) { list_push((FreeNode**)&A->free_lists[k-1], (FreeNode*)block); break; }
    }
}
