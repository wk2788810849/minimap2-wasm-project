#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "kalloc.h"
#include "memory_aligned.h"

typedef union align_unit_t {
    struct {
        size_t size;
        union align_unit_t *ptr;
    } s;
    long double alignment_enforcer;
} align_unit_t;

typedef struct {
    void *par;
    size_t min_core_size;
    align_unit_t base, *loop_head, *core_head; 
} kmem_t;

#define PTR_TO_UINT(p) ((uintptr_t)(p))
#define UINT_TO_PTR(u) ((void*)(u))

static void panic(const char *s)
{
    fprintf(stderr, "%s\n", s);
}

void *km_init2(void *km_par, size_t min_core_size)
{
    kmem_t *km;
    km = (kmem_t*)kcalloc(km_par, 1, sizeof(kmem_t));
    km->par = km_par;
    if (km_par) km->min_core_size = min_core_size > 0? min_core_size : ((kmem_t*)km_par)->min_core_size - 2;
    else km->min_core_size = min_core_size > 0? min_core_size : 0x80000;
    return (void*)km;
}

void *km_init(void) { return km_init2(0, 0); }

void km_destroy(void *_km)
{
    kmem_t *km = (kmem_t*)_km;
    void *km_par;
    align_unit_t *p, *q;
    if (km == NULL) return;
    km_par = km->par;
    for (p = km->core_head; p != NULL;) {
        q = p->s.ptr;
        kfree(km_par, p);
        p = q;
    }
    kfree(km_par, km);
}

static align_unit_t *morecore(kmem_t *km, size_t nu)
{
    align_unit_t *q,*header;
    size_t bytes;
    
    nu = nu < 1 ? 1 : nu;
    nu = ((nu + km->min_core_size - 1) / km->min_core_size) * km->min_core_size;
    bytes = nu * sizeof(align_unit_t);

    q = (align_unit_t*)calloc(1, bytes);
    if (!q) {
        fprintf(stderr, "[morecore] calloc failed for %zu bytes\n", bytes);
        return NULL;
    }
    
    q->s.ptr = km->core_head;
    q->s.size = nu;
    km->core_head = q;

    header = q + 1;
    header->s.size = nu - 1;
    header->s.ptr = header;
    
    kfree(km, (uint8_t*)header + sizeof(align_unit_t));
    
    return km->loop_head;
}

void kfree(void *_km, void *ap)
{
    align_unit_t *p, *q;
    kmem_t *km = (kmem_t*)_km;
    
    if (!ap) return;
    if (km == NULL) {
        free(ap);
        return;
    }
    
    p = (align_unit_t*)((uint8_t*)ap - sizeof(align_unit_t));

    uintptr_t p_uint = PTR_TO_UINT(p);
    uintptr_t p_end_uint = p_uint + p->s.size * sizeof(align_unit_t);
    if (!km->loop_head) return;

    for (q = km->loop_head;; q = q->s.ptr) {
        uintptr_t q_uint = PTR_TO_UINT(q);
        uintptr_t q_ptr_uint = PTR_TO_UINT(q->s.ptr);
        uintptr_t q_end_uint = q_uint + q->s.size * sizeof(align_unit_t);
        
        int cond1 = (p_uint > q_uint && p_uint < q_ptr_uint);
        int cond2 = (q_uint >= q_ptr_uint && (p_uint > q_uint || p_uint < q_ptr_uint));
        int cond3 = (q_ptr_uint - q_uint) > km->min_core_size;
        if (cond1 || cond2 || cond3) break;
        if (q->s.ptr == km->loop_head) break;
    }

    uintptr_t q_ptr_uint = PTR_TO_UINT(q->s.ptr);
    if (p_end_uint == q_ptr_uint) {
        p->s.size += q->s.ptr->s.size;
        p->s.ptr = q->s.ptr->s.ptr;
    } else if (p_end_uint > q_ptr_uint && q_ptr_uint >= p_uint) {
        if ((p_end_uint - q_ptr_uint) > 16) {
            fprintf(stderr, "[WARNING] kfree: allocated block overlaps free block (skip panic) - p_end=0x%lx, q_ptr=0x%lx\n", p_end_uint, q_ptr_uint);
        }
        return;
    } else {
        p->s.ptr = q->s.ptr;
    }

    uintptr_t q_end_uint = PTR_TO_UINT(q) + q->s.size * sizeof(align_unit_t);
    if (q_end_uint == p_uint) {
        q->s.size += p->s.size;
        q->s.ptr = p->s.ptr;
        km->loop_head = q;
    } else if (q_end_uint > p_uint && p_uint >= PTR_TO_UINT(q)) {
        if ((q_end_uint - p_uint) > 16) {
            fprintf(stderr, "[WARNING] kfree: free block overlaps allocated block (skip panic) - q_end=0x%lx, p=0x%lx\n", q_end_uint, p_uint);
        }
        km->loop_head = p;
        q->s.ptr = p;
    } else {
        km->loop_head = p;
        q->s.ptr = p;
    }
}

void *kmalloc(void *_km, size_t n_bytes)
{
    kmem_t *km = (kmem_t*)_km;
    size_t n_units;
    align_unit_t *p, *q;

    if (n_bytes == 0) return NULL;
    
    if (km == NULL) {
        return realloc_aligned(NULL, 0, n_bytes, 16);
    }
    
    n_units = (n_bytes + sizeof(align_unit_t) - 1) / sizeof(align_unit_t);
    n_units = n_units < 1 ? 1 : n_units;
    n_units += 1;

    if (!km->loop_head) {
        km->base.s.size = 0;
        km->base.s.ptr = &km->base;
        km->loop_head = &km->base;
    }
    q = km->loop_head;

    for (p = q->s.ptr;; q = p, p = p->s.ptr) {
        if (p->s.size == 0) {
            if (p == km->loop_head) {
                if ((p = morecore(km, n_units)) == NULL) {
                    fprintf(stderr, "[kmalloc] morecore failed for %zu bytes\n", n_bytes);
                    return NULL;
                }
                q = km->loop_head;
                p = q->s.ptr;
            }
            continue;
        }

        if (p->s.size >= n_units) {
            align_unit_t *alloc_block = NULL;
            
            if (p->s.size == n_units) {
                q->s.ptr = p->s.ptr;
                alloc_block = p;
            } else {
                p->s.size -= n_units;
                alloc_block = p + p->s.size;
                alloc_block->s.size = n_units;
                alloc_block->s.ptr = p->s.ptr;
            }

            km->loop_head = q;
            if (!alloc_block) {
                fprintf(stderr, "[kmalloc] alloc_block is NULL for %zu bytes\n", n_bytes);
                return NULL;
            }
            
            void *user_ptr = (uint8_t*)alloc_block + sizeof(align_unit_t);
            if (PTR_TO_UINT(user_ptr) > PTR_TO_UINT(km) + 0x10000000) {
                fprintf(stderr, "[kmalloc] pointer out of range: %p\n", user_ptr);
                return NULL;
            }
            return user_ptr;
        }

        if (p == km->loop_head) {
            if ((p = morecore(km, n_units)) == NULL) {
                fprintf(stderr, "[kmalloc] morecore failed for %zu bytes\n", n_bytes);
                return NULL;
            }
        }
    }
}

void *kcalloc(void *_km, size_t count, size_t size)
{
    kmem_t *km = (kmem_t*)_km;
    void *p;
    if (size == 0 || count == 0) return 0;
    if (km == NULL) return calloc(count, size);
    p = kmalloc(km, count * size);
    memset(p, 0, count * size);
    return p;
}

void *krealloc(void *_km, void *ap, size_t n_bytes)
{
    kmem_t *km = (kmem_t*)_km;
    size_t cap;
    void *new_p;

    if (n_bytes == 0) {
        kfree(km, ap); return 0;
    }
    if (km == NULL) return realloc(ap, n_bytes);
    if (ap == NULL) return kmalloc(km, n_bytes);
    
    align_unit_t *header = (align_unit_t*)((uint8_t*)ap - sizeof(align_unit_t));
    cap = header->s.size * sizeof(align_unit_t) - sizeof(align_unit_t);

    if (cap >= n_bytes) return ap;

    new_p = kmalloc(km, n_bytes);
    if (new_p) memcpy(new_p, ap, cap);
    kfree(km, ap);
    return new_p;
}

void *krelocate(void *km, void *ap, size_t n_bytes)
{
    void *p;
    if (km == 0 || ap == 0) return ap;
    p = kmalloc(km, n_bytes);
    memcpy(p, ap, n_bytes);
    kfree(km, ap);
    return p;
}

void km_stat(const void *_km, km_stat_t *s)
{
    kmem_t *km = (kmem_t*)_km;
    align_unit_t *p;
    memset(s, 0, sizeof(km_stat_t));
    if (km == NULL || km->loop_head == NULL) return;
    for (p = km->loop_head;; p = p->s.ptr) {
        s->available += p->s.size * sizeof(align_unit_t);
        if (p->s.size != 0) ++s->n_blocks;
        if (p->s.ptr > p && p + p->s.size > p->s.ptr)
            panic("[km_stat] The end of a free block enters another free block.");
        if (p->s.ptr == km->loop_head) break;
    }
    for (p = km->core_head; p != NULL; p = p->s.ptr) {
        size_t size = p->s.size * sizeof(align_unit_t);
        ++s->n_cores;
        s->capacity += size;
        s->largest = s->largest > size? s->largest : size;
    }

    s->n_blocks = s->n_cores;
}

void km_stat_print(const void *km)
{
    km_stat_t st;
    km_stat(km, &st);
    fprintf(stderr, "[km_stat] cap=%ld, avail=%ld, largest=%ld, n_core=%ld, n_block=%ld\n",
            st.capacity, st.available, st.largest, st.n_blocks, st.n_cores);
}