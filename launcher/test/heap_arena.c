/*=============================================================================
 * A first-fit arena allocator that stands in for malloc/calloc/realloc/free
 * in the HOST test build, sized to what this project's device profile says
 * is actually free once the framebuffer is carved out (device_profiles/
 * esp32c6.sh's DP_FREE_HEAP_BYTES) - see docs/Sand/Performance-Tuning-
 * Attempts.md's "recurring failure modes": a fixture that fits comfortably
 * in a laptop's gigabytes has twice now turned out to be impossible on the
 * board, and nothing on the host caught it before a whole capture cycle
 * was spent finding out.
 *
 * FIRST-FIT WITH REAL FRAGMENTATION is the point. This is not a byte
 * counter that fails a fixture once some running total crosses a line - it
 * is a doubly-linked list of address-ordered blocks, and an allocation
 * fails exactly when no ONE free block is big enough, even if the sum of
 * several is. That is the same rule the device's own allocator runs under,
 * and it is the difference that matters: a 41 KB grid can fail on a heap
 * with 50 KB free but no block over 38 KB, which a total-bytes check would
 * wave through.
 *
 * Deliberately simple: one header per block (prev/next/size/in_use/magic),
 * first-fit search, split on allocate, coalesce-both-directions on free.
 * This is a test gate, not something a real program should link against.
 *
 * MECHANISM: the host link adds -Wl,--wrap=malloc (and calloc/realloc/free)
 * so every call to those names in the test binary resolves to __wrap_*
 * here instead, with the real libc entry points reachable as __real_*.
 *
 * THE ONE CAVEAT THAT MATTERS: libc-internal allocations do not reliably
 * route through the wrapper. Measured on this toolchain (MinGW-w64
 * x86_64-ucrt gcc 16.1.0): a pointer allocated inside strdup() arrived at
 * __wrap_free() having never been seen by __wrap_malloc() - the CRT's own
 * strdup calls its own already-resolved reference to malloc, not the
 * import our --wrap redirects. So every function below that receives a
 * pointer (free, realloc) MUST tell an arena pointer from a foreign one
 * before touching any block header, and forward the foreign case to
 * __real_free / __real_realloc untouched. Getting this backwards means
 * reading three bytes past a strdup'd string as if they were {prev, next,
 * size} - which is exactly as bad as it sounds.
 *
 * Everything in this file is compiled in ONLY when HOST_HEAP_ARENA is
 * defined. launcher/test/timing.c is also compiled into the device
 * firmware (main/CMakeLists.txt) and into the sand perf_probe harness
 * (main/apps/sand/tools/perf_probe/build_probe.sh); neither defines this
 * macro, and this file is never even added to either of their source
 * lists, so neither sees so much as this file existing.
 *===========================================================================*/
#ifdef HOST_HEAP_ARENA

#include "heap_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_HEAP_ARENA_BYTES
#error "heap_arena.c: HOST_HEAP_ARENA_BYTES must be supplied by the build " \
    "(-DHOST_HEAP_ARENA_BYTES=<n>), sourced from a device profile's " \
    "DP_FREE_HEAP_BYTES (launcher/tools/device_profiles/esp32c6.sh, read " \
    "via launcher/tools/device_profile.sh). A gate running against an " \
    "invented cap is worse than no gate."
#endif

/* Static storage is sized well above HOST_HEAP_ARENA_BYTES so the runtime
 * override below can WIDEN the cap for an experiment without a rebuild -
 * HOST_HEAP_ARENA_BYTES is only the compile-time DEFAULT. 4 MiB costs
 * nothing in a host test binary's .bss. */
#ifndef HOST_HEAP_ARENA_STORAGE_BYTES
#define HOST_HEAP_ARENA_STORAGE_BYTES (4u * 1024u * 1024u)
#endif

/* _Alignas rather than a plain unsigned char[] - a static array has no
 * alignment guarantee stronger than 1 byte in the standard, and every
 * block header below assumes it can place an arena_block_t at the base. */
static _Alignas(max_align_t) unsigned char s_storage[HOST_HEAP_ARENA_STORAGE_BYTES];

typedef struct arena_block
{
    struct arena_block *prev;
    struct arena_block *next;
    size_t size; /* usable payload bytes, excludes this header */
    int in_use;
    unsigned magic; /* set while in_use, checked on free() - catches a
                      * double-free or a foreign pointer that happened to
                      * land inside the arena's byte range */
} arena_block_t;

#define ARENA_MAGIC_LIVE 0xA23EA11Cu
#define ARENA_ALIGN (sizeof(max_align_t))
#define ARENA_HEADER_SIZE (align_up(sizeof(arena_block_t), ARENA_ALIGN))

static arena_block_t *s_head;
static size_t s_cap;      /* effective cap in bytes, <= sizeof(s_storage) */
static size_t s_cur_bytes;  /* outstanding payload bytes right now */
static size_t s_cur_blocks; /* outstanding block count right now */
static size_t s_peak_bytes; /* highest s_cur_bytes since the last reset */
static int s_initialized;

static size_t align_up(size_t n, size_t a)
{
    /* a is always a power of two here (sizeof(max_align_t)) */
    return (n + (a - 1)) & ~(a - 1);
}

static int ptr_in_arena(const void *p)
{
    const unsigned char *b = (const unsigned char *)p;
    return b >= s_storage && b < s_storage + sizeof(s_storage);
}

/* Reads HOST_HEAP_ARENA_BYTES (the compile-time default) or, if set, the
 * HOST_HEAP_ARENA_BYTES environment variable - so a one-off experiment can
 * widen or narrow the cap without a rebuild. Prints the effective cap and
 * where it came from exactly once, since a gate whose cap is silently
 * different from what the last person read in the log is worse than one
 * that never widened at all. */
static size_t arena_effective_cap(void)
{
    size_t cap = (size_t)HOST_HEAP_ARENA_BYTES;
    const char *origin = "compile-time default (device profile "
                          "DP_FREE_HEAP_BYTES via -DHOST_HEAP_ARENA_BYTES)";

    const char *env = getenv("HOST_HEAP_ARENA_BYTES");
    if (env && *env)
    {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && *end == '\0' && v > 0)
        {
            cap = (size_t)v;
            origin = "environment override (HOST_HEAP_ARENA_BYTES)";
        }
        else
        {
            fprintf(stderr, "heap_arena: ignoring unparseable HOST_HEAP_ARENA_BYTES=%s\n", env);
        }
    }

    if (cap > sizeof(s_storage))
    {
        fprintf(stderr,
                "heap_arena: requested cap %zu exceeds static storage %zu, clamping\n",
                cap, sizeof(s_storage));
        cap = sizeof(s_storage);
    }

    printf("heap_arena: effective cap = %zu bytes (%s)\n", cap, origin);
    return cap;
}

static void arena_init_once(void)
{
    if (s_initialized)
    {
        return;
    }
    s_cap = arena_effective_cap();
    s_head = (arena_block_t *)s_storage;
    s_head->prev = NULL;
    s_head->next = NULL;
    s_head->size = s_cap > ARENA_HEADER_SIZE ? s_cap - ARENA_HEADER_SIZE : 0;
    s_head->in_use = 0;
    s_head->magic = 0;
    s_initialized = 1;
}

/* Splits and hands out the free block b (already known to be big enough),
 * leaving the remainder as a new free block when there is enough of it to
 * be worth a header - a remainder smaller than one more header is folded
 * into this allocation instead of stranding an unusable sliver. */
static void *arena_take_block(arena_block_t *b, size_t need)
{
    size_t remaining = b->size - need;
    if (remaining >= ARENA_HEADER_SIZE + ARENA_ALIGN)
    {
        arena_block_t *nb = (arena_block_t *)((unsigned char *)b + ARENA_HEADER_SIZE + need);
        nb->size = remaining - ARENA_HEADER_SIZE;
        nb->in_use = 0;
        nb->magic = 0;
        nb->prev = b;
        nb->next = b->next;
        if (nb->next)
        {
            nb->next->prev = nb;
        }
        b->next = nb;
        b->size = need;
    }
    b->in_use = 1;
    b->magic = ARENA_MAGIC_LIVE;
    s_cur_bytes += b->size;
    s_cur_blocks += 1;
    if (s_cur_bytes > s_peak_bytes)
    {
        s_peak_bytes = s_cur_bytes;
    }
    return (unsigned char *)b + ARENA_HEADER_SIZE;
}

/* First-fit search plus, on failure, the same story a device OOM would
 * give (bd esp32c6-e82: "41.2 KiB needed, 38 KiB largest free block") -
 * printed here rather than left for the caller to reconstruct from a bare
 * NULL. */
static void *arena_alloc(size_t n)
{
    arena_init_once();
    if (n == 0)
    {
        n = 1; /* malloc(0): return a distinct, freeable pointer, not NULL */
    }
    size_t need = align_up(n, ARENA_ALIGN);

    arena_block_t *b;
    for (b = s_head; b; b = b->next)
    {
        if (!b->in_use && b->size >= need)
        {
            return arena_take_block(b, need);
        }
    }

    size_t total_free = 0, largest_free = 0, free_blocks = 0;
    for (arena_block_t *s = s_head; s; s = s->next)
    {
        if (!s->in_use)
        {
            total_free += s->size;
            free_blocks += 1;
            if (s->size > largest_free)
            {
                largest_free = s->size;
            }
        }
    }
    fprintf(stderr,
            "heap_arena: alloc of %zu bytes FAILED - %zu needed, %zu bytes "
            "largest free block, %zu bytes free across %zu block(s), cap %zu\n",
            n, need, largest_free, total_free, free_blocks, s_cap);
    return NULL;
}

/* Merges b with a free neighbour on either side. Address-ordered list, so
 * "neighbour" here means adjacent in the list, which is also adjacent in
 * memory by construction (every split creates its remainder immediately
 * after the block it came from). */
static void arena_release(arena_block_t *b)
{
    b->in_use = 0;
    b->magic = 0;
    s_cur_bytes -= b->size;
    s_cur_blocks -= 1;

    if (b->next && !b->next->in_use)
    {
        arena_block_t *n = b->next;
        b->size += ARENA_HEADER_SIZE + n->size;
        b->next = n->next;
        if (b->next)
        {
            b->next->prev = b;
        }
    }
    if (b->prev && !b->prev->in_use)
    {
        arena_block_t *p = b->prev;
        p->size += ARENA_HEADER_SIZE + b->size;
        p->next = b->next;
        if (p->next)
        {
            p->next->prev = p;
        }
    }
}

/* Caller must already know ptr is inside the arena - see ptr_in_arena()
 * calls in the wrappers below. Aborts on a bad header instead of silently
 * corrupting the list: a double-free or an in-arena-but-not-a-live-block
 * pointer is a real bug, and a test gate that swallows it defeats the
 * point of running under a byte-for-byte-accurate allocator at all. */
static void arena_free(void *ptr)
{
    arena_block_t *b = (arena_block_t *)((unsigned char *)ptr - ARENA_HEADER_SIZE);
    if (!ptr_in_arena(b) || b->magic != ARENA_MAGIC_LIVE)
    {
        fprintf(stderr,
                "heap_arena: free(%p) does not look like a live arena "
                "block - double free, corruption, or a pointer this arena "
                "never issued\n",
                ptr);
        abort();
    }
    arena_release(b);
}

void heap_arena_snapshot(size_t *out_blocks, size_t *out_bytes)
{
    arena_init_once();
    if (out_blocks)
    {
        *out_blocks = s_cur_blocks;
    }
    if (out_bytes)
    {
        *out_bytes = s_cur_bytes;
    }
}

size_t heap_arena_peak_bytes(void)
{
    return s_peak_bytes;
}

void heap_arena_reset_peak(void)
{
    arena_init_once();
    /* Floored at what's already outstanding, not zeroed - a test that
     * starts after an earlier leak should show that leak weighing on its
     * own peak, the same way it would starve a real boot. */
    s_peak_bytes = s_cur_bytes;
}

/* --- malloc/calloc/realloc/free interposition ------------------------- */

extern void *__real_malloc(size_t size);
extern void __real_free(void *ptr);
extern void *__real_realloc(void *ptr, size_t size);

void *__wrap_malloc(size_t size)
{
    return arena_alloc(size);
}

void __wrap_free(void *ptr)
{
    if (!ptr)
    {
        return;
    }
    if (!ptr_in_arena(ptr))
    {
        /* Almost certainly a libc-internal allocation (strdup() and
         * friends) that never went through __wrap_malloc - see this
         * file's top comment. Forward it rather than misread foreign
         * bytes as one of our headers. */
        __real_free(ptr);
        return;
    }
    arena_free(ptr);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
    {
        return NULL; /* overflow - same contract calloc itself makes */
    }
    size_t total = nmemb * size;
    void *p = arena_alloc(total);
    if (p)
    {
        memset(p, 0, total);
    }
    return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr)
    {
        return arena_alloc(size);
    }
    if (!ptr_in_arena(ptr))
    {
        /* Foreign pointer - see __wrap_free above for why this can happen
         * at all. Hand it to the real realloc untouched. */
        return __real_realloc(ptr, size);
    }
    if (size == 0)
    {
        arena_free(ptr);
        return NULL;
    }

    arena_block_t *b = (arena_block_t *)((unsigned char *)ptr - ARENA_HEADER_SIZE);
    size_t need = align_up(size, ARENA_ALIGN);
    if (need <= b->size)
    {
        /* Shrinking (or same size) in place. No split on shrink - kept
         * simple on purpose, this is a test gate, not a production
         * allocator, and the extra fragmentation from not splitting here
         * only ever makes a fixture's job HARDER, never easier, so it
         * cannot hide a device failure. */
        return ptr;
    }

    void *grown = arena_alloc(size);
    if (!grown)
    {
        return NULL; /* realloc's own contract: leave the original intact */
    }
    memcpy(grown, ptr, b->size);
    arena_free(ptr);
    return grown;
}

#endif /* HOST_HEAP_ARENA */
