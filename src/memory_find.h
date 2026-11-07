/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GOW3_MEMORY_FIND_H
#define GOW3_MEMORY_FIND_H
/* runtime_memory_find_all's aligned pass (src/runtime_memory.c), without side effects for the
 * tests: every block whole in [lo,hi) that starts at a MEMORY_FIND_ALIGN boundary, all blocks
 * looked for in one pass by the 8 bytes at their signature (sig_at, a multiple of the alignment),
 * then the signature's 64 bytes and the whole block. Hits are appended after the n already found
 * (addresses, which = the block's index), up to max; returns the new count. The GPU reads a
 * texture from a 256-byte aligned address (a T# holds its base in 256-byte units). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MEMORY_FIND_ALIGN=256, MEMORY_FIND_MAX_BLOCKS=16 };

static inline int memory_find_aligned(uintptr_t lo, uintptr_t hi, const unsigned char *const *blocks,
                                      const size_t *sizes, const size_t *sig_at, int count,
                                      uintptr_t *addresses, int *which, int n, int max) {
    uint64_t key[MEMORY_FIND_MAX_BLOCKS];
    int use[MEMORY_FIND_MAX_BLOCKS], any=0;
    if (count>MEMORY_FIND_MAX_BLOCKS) count=MEMORY_FIND_MAX_BLOCKS;
    for (int b=0;b<count;++b) {
        use[b]=blocks[b] && sizes[b]>=sig_at[b]+64 && sig_at[b]%MEMORY_FIND_ALIGN==0;
        if (use[b]) { memcpy(&key[b],blocks[b]+sig_at[b],8); any=1; }
    }
    if (!any) return n;
    uintptr_t p=(lo+MEMORY_FIND_ALIGN-1) & ~(uintptr_t)(MEMORY_FIND_ALIGN-1);
    while (n<max && p<hi && hi-p>=64) {
        uint64_t v;
        memcpy(&v,(const void *)p,8);
        uintptr_t next=p+MEMORY_FIND_ALIGN;
        for (int b=0;b<count;++b) {
            if (!use[b] || v!=key[b] || p-lo<sig_at[b]) continue;
            const uintptr_t base=p-sig_at[b];
            if (hi-base<sizes[b] || memcmp((const void *)p,blocks[b]+sig_at[b],64) ||
                memcmp((const void *)base,blocks[b],sizes[b])) continue;
            addresses[n]=base;
            which[n++]=b;
            /* A next copy starts after this one: its signature is past this block's end. */
            next=(base+sizes[b]+MEMORY_FIND_ALIGN-1) & ~(uintptr_t)(MEMORY_FIND_ALIGN-1);
            if (next<=p) next=p+MEMORY_FIND_ALIGN;
            break;
        }
        p=next;
    }
    return n;
}
#endif
