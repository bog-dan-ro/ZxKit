/* Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later */
/* Linux/glibc diagnostic interposer. Counts requested bytes, not live heap size.
 * Build: cc -shared -fPIC -O2 tools/profiling/profile_allocations.c -o build/profiling/allocations.so
 * Run separately from production CPU sampling with LD_PRELOAD and PROFILE_WARMUP.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void __libc_free(void *);
static _Atomic uint64_t calls, bytes, frees;
static _Atomic int ready;
static double begin;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
__attribute__((constructor)) static void start(void)
{
    const char *warmup = getenv("PROFILE_WARMUP");
    begin = now() + (warmup ? atof(warmup) : 5.0);
    ready = 1;
}
static void count(size_t size)
{
    if (ready && now() >= begin) { atomic_fetch_add(&calls, 1); atomic_fetch_add(&bytes, size); }
}
void *malloc(size_t size) { count(size); return __libc_malloc(size); }
void *calloc(size_t n, size_t size) { count(n * size); return __libc_calloc(n, size); }
void *realloc(void *p, size_t size) { count(size); return __libc_realloc(p, size); }
void free(void *p) { if (ready && p && now() >= begin) atomic_fetch_add(&frees, 1); __libc_free(p); }
void *aligned_alloc(size_t align, size_t size) { count(size); return __libc_memalign(align, size); }
int posix_memalign(void **p, size_t align, size_t size)
{
    if (align < sizeof(void *) || (align & (align - 1))) return 22;
    count(size); *p = __libc_memalign(align, size); return *p ? 0 : 12;
}
__attribute__((destructor)) static void finish(void)
{
    ready = 0;
    fprintf(stderr, "PROFILE_ALLOCATIONS {\"seconds_after_warmup\":%.6f,\"calls\":%llu,\"requested_bytes\":%llu,\"frees\":%llu}\n",
            now() - begin, (unsigned long long)calls, (unsigned long long)bytes, (unsigned long long)frees);
}
