/*
 * Flush the host cpu caches.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_CACHEFLUSH_H
#define QEMU_CACHEFLUSH_H

/**
 * flush_idcache_range:
 * @rx: instruction address
 * @rw: data address
 * @len: length to flush
 *
 * Flush @len bytes of the data cache at @rw and the icache at @rx
 * to bring them in sync.  The two addresses may be different virtual
 * mappings of the same physical page(s).
 */

#if defined(__i386__) || defined(__x86_64__) || defined(__s390__)

static inline void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len)
{
    /* icache is coherent and does not require flushing. */
}

#elif defined(EMSCRIPTEN)

static inline void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len)
{
    /* Wasm doesn't have executable region of memory. */
}

#else

void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len);

#endif

/**
 * flush_idcache_defer_begin, flush_idcache_defer_end:
 * @b: caller-owned queue, used by one thread at a time
 *
 * Between the two calls, this thread's flush_idcache_range() calls are
 * queued in @b, then run at _end (or when @b fills) with one barrier
 * sequence for the whole batch instead of one per call. Code written in
 * the window must not run, on any thread, before _end returns. Hosts
 * with coherent caches or without a batched sequence flush at once.
 */
#if defined(__aarch64__) && !defined(CONFIG_WIN32)

#define FLUSH_IDCACHE_BATCH_MAX 256

typedef struct FlushIdcacheBatch {
    size_t n;
    struct {
        uintptr_t rx, rw;
        size_t len;
    } e[FLUSH_IDCACHE_BATCH_MAX];
} FlushIdcacheBatch;

void flush_idcache_defer_begin(FlushIdcacheBatch *b);
void flush_idcache_defer_end(FlushIdcacheBatch *b);

#else

typedef struct FlushIdcacheBatch {
    char unused;
} FlushIdcacheBatch;

static inline void flush_idcache_defer_begin(FlushIdcacheBatch *b) { }
static inline void flush_idcache_defer_end(FlushIdcacheBatch *b) { }

#endif

#endif /* QEMU_CACHEFLUSH_H */
