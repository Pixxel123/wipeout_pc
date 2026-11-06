/* SPDX-License-Identifier: GPL-2.0-or-later */
/* runtime_heaptrace.c's hooks without the game (#150): a fake image with the GPU heap's free
 * prologue and alloc vtable at the real offsets, installed as probe.c does it (image read/write,
 * then the segment pass makes the code read/execute). A free of another heap must run the
 * original through the trampoline with its arguments; a free of heap A must reach the texture
 * cache. Linux only for now: the install is where the platforms differ. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

void runtime_heap_trace_install(unsigned char *image, uint64_t size);

enum { BASE = 0x1000000, VTABLE = 0x19977f0, HANDLE_A = 0x2212548, HEAP_A = 0x2212568,
       ALLOC_FN = 0x17f0af0, FREE_IMPL = 0x17f0f00, CALLER = 0x1500000 };
static volatile uint64_t original_free_runs, original_free_rdi, original_free_rsi;
static uint64_t freed_address, freed_size, freed_calls;
void gow3gpu_guest_heap_freed(uintptr_t address, uint64_t size) {
    freed_address = address; freed_size = size; ++freed_calls;
}
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main(void) {
    const uint64_t size = HEAP_A - BASE + 0x1000;
    unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *f = image + (FREE_IMPL - BASE), *p = f;
    /* the real prologue: push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12 */
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x41, 0x55, 0x41, 0x54};
    memcpy(p, prologue, sizeof(prologue)); p += sizeof(prologue);
    /* record the call and its arguments, which must survive the trampoline */
    uint64_t at = (uint64_t)&original_free_rdi;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &at, 8); p += 8; *p++ = 0x48; *p++ = 0x89; *p++ = 0x38; /* mov [rax],rdi */
    at = (uint64_t)&original_free_rsi;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &at, 8); p += 8; *p++ = 0x48; *p++ = 0x89; *p++ = 0x30; /* mov [rax],rsi */
    at = (uint64_t)&original_free_runs;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &at, 8); p += 8; *p++ = 0xf0; *p++ = 0x48; *p++ = 0xff; *p++ = 0x00;
    static const unsigned char epilogue[] = {0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
    memcpy(p, epilogue, sizeof(epilogue));
    /* the alloc: return a fixed address (heap A's own placement is used once it has a size) */
    unsigned char *a = image + (ALLOC_FN - BASE);
    const uint64_t fixed = 0x7000000;
    a[0] = 0x48; a[1] = 0xb8; memcpy(a + 2, &fixed, 8); a[10] = 0xc3;
    const uint64_t alloc_ptr = (uint64_t)a;
    memcpy(image + (VTABLE - BASE) + 0x10, &alloc_ptr, 8);
    /* heap A: 256 MiB at 0x10000000 */
    const uint64_t heap_base = 0x10000000, heap_total = 256u << 20;
    memcpy(image + (HEAP_A - BASE), &heap_base, 8);
    memcpy(image + (HEAP_A - BASE) + 0x10, &heap_total, 8);

    runtime_heap_trace_install(image, size);
    mprotect(image, size, PROT_READ | PROT_EXEC); /* the segment pass: code RX, data RW */
    mprotect(image + ((VTABLE - BASE) & ~0xfffull), size - ((VTABLE - BASE) & ~0xfffull), PROT_READ | PROT_WRITE);
    uint64_t hooked;
    memcpy(&hooked, image + (VTABLE - BASE) + 0x10, 8);
    CHECK(hooked != alloc_ptr);                      /* vtable now points at traced_alloc */
    CHECK(f[0] == 0x48 && f[1] == 0xb8 && f[10] == 0xff && f[11] == 0xe0); /* free patched */

    typedef uint64_t (*Alloc)(void *, uint64_t, uint64_t, uint64_t);
    typedef void (*Free)(void *, uint64_t);
    Free game_free = (Free)(void *)f;
    Alloc game_alloc = (Alloc)hooked;

    /* 1. another heap's free: the original runs through the trampoline, with its arguments */
    game_free((void *)0x1234, 0x5678);
    CHECK(original_free_runs == 1);
    CHECK(original_free_rdi == 0x1234 && original_free_rsi == 0x5678);
    CHECK(freed_calls == 0);

    /* 2. another handle's alloc goes to the game's own alloc */
    CHECK(game_alloc((void *)0x999, 4096, 256, 0) == fixed);

    /* 3. heap A: placed by the hook inside the heap, and its free reaches the texture cache */
    void *handle_a = image + (HANDLE_A - BASE), *heap_a = image + (HEAP_A - BASE);
    const uint64_t block = game_alloc(handle_a, 1 << 20, 0x10000, 0);
    CHECK(block >= heap_base && block + (1 << 20) <= heap_base + heap_total && block % 0x10000 == 0);
    game_free(heap_a, block);
    CHECK(freed_calls == 1 && freed_address == block && freed_size == (1 << 20));
    CHECK(original_free_runs == 1); /* heap A's free does not reach the game's trees */

    /* 4. the freed space is reused */
    CHECK(game_alloc(handle_a, 1 << 20, 0x10000, 0) == block);

    printf(failures ? "%d FAILED\n" : "all heap hook checks passed\n", failures);
    return failures != 0;
}
