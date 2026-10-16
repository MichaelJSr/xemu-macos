/*
 * xemu checkpoint RAM: guest RAM images, and the dirty log behind
 * incremental restores and saves against a base
 *
 * A checkpoint keeps guest RAM as an image: a pointer per target page of
 * every migratable RAM block to a copy of the page, NULL for a zero page.
 * An image saved against a base starts from the base's pointers and
 * copies only the pages that differ, so it shares the rest of the base's
 * copies (refcounted chunks, which outlive a deleted base).
 *
 * RAM equals one image right after a save or a restore (ram.synced).
 * From then on a page can differ from another image only if something
 * wrote it or the two images hold it differently, so a restore or a save
 * against a base that knows the writes compares only those pages. The
 * writes come from QEMU's migration dirty log (DIRTY_MEMORY_MIGRATION),
 * which runs under GLOBAL_DIRTY_XEMU once an incremental restore or a
 * save against a base asks for it, until the last checkpoint goes:
 *  - TCG stores into a clean page trap once (TLB_NOTDIRTY), REP bulk
 *    copies and the page walker's accessed/dirty bits included; the
 *    sub-page prefilter skips the trap only while the dirty bits it
 *    reads are all set, and the log clears every block's;
 *  - DMA (IDE, USB, nvnet), DSP DMA (memory_region_set_dirty), reset's
 *    ROM copies and gdbstub writes go through the address space, which
 *    marks the log while it runs;
 *  - stores straight into RAM that skip it mark through
 *    memory_region_set_client_dirty while the log runs: the APU's voice
 *    and notifier stores (unmarked otherwise on Apple), nv2a semaphore
 *    releases and reports, and renderer surface write-backs and blits;
 *  - the small blocks (BIOS, MCPX shadow, nv2a RAMIN, which PFIFO writes
 *    through its own pointer) are compared whole every time.
 * Migration takes the log's bits too, so they are copied first
 * (log_sync_global). A load that isn't a checkpoint restore (loadvm, an
 * incoming migration) or a restore that fails leaves RAM unknown, and the
 * next one compares every page. Not seen: another process writing guest
 * RAM through XEMU_RAM_SHM's mapping.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/cpu-common.h"
#include "exec/ramlist.h"
#include "exec/target_page.h"
#include "migration/misc.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/ramblock.h"
#include "xemu-checkpoints.h"

/* Blocks up to this size are compared whole instead of trusting the log. */
#define SMALL_BLOCK (4 * MiB)

typedef struct Block {
    RAMBlock *rb;
    uint8_t *host;
    ram_addr_t offset;      /* ram_addr_t of its first byte */
    uint64_t first;         /* index of its first page in an image */
    uint64_t pages;
    bool small;
    unsigned long *dirty;   /* pages written since ram.synced */
} Block;

/*
 * Page copies come from mmap, not the heap: Apple's malloc keeps a freed
 * large block resident for good, so a deleted checkpoint's 70 MiB stayed
 * in the footprint.
 */
typedef struct Chunk {
    int refs;
    uint8_t *data;
    size_t size;
} Chunk;

/*
 * A chunk's mapping: whole host pages, which target pages need not fill.
 * qemu_ram_mmap unmaps its alignment slack after the mapping's end, and
 * munmap refuses an address off a page boundary (a Linux host with pages
 * over 4 KiB would leave up to 2 MiB reserved per chunk).
 */
static size_t chunk_map_size(size_t size)
{
    return ROUND_UP(size, qemu_real_host_page_size());
}

struct XemuRamImage {
    int refs;
    const uint8_t **page;   /* every block's pages; NULL is a zero page */
    GPtrArray *chunks;      /* the Chunks its pages point into, one ref each */
    uint64_t own;           /* pages it copied itself */
};

static struct {
    Block *blocks;
    unsigned nblocks;
    uint64_t pages;
    size_t page_size;
    bool on;                /* the GLOBAL_DIRTY_XEMU log runs */
    bool restoring;         /* a checkpoint restore is loading */
    XemuRamImage *synced;   /* RAM equalled it then, unless NULL */
    MemoryListener listener;
} ram;

static void blocks_init(void)
{
    g_autoptr(GArray) a = g_array_new(false, true, sizeof(Block));
    RAMBlock *rb;

    if (ram.blocks) {
        return;
    }
    ram.page_size = qemu_target_page_size();
    WITH_RCU_READ_LOCK_GUARD() {
        RAMBLOCK_FOREACH(rb) {
            if (migrate_ram_is_ignored(rb)) {
                continue;
            }
            Block b = {
                .rb = rb,
                .host = qemu_ram_get_host_addr(rb),
                .offset = qemu_ram_get_offset(rb),
                .first = ram.pages,
                .pages = qemu_ram_get_used_length(rb) / ram.page_size,
            };
            b.small = qemu_ram_get_used_length(rb) <= SMALL_BLOCK;
            b.dirty = bitmap_new(b.pages);
            ram.pages += b.pages;
            g_array_append_val(a, b);
        }
    }
    ram.nblocks = a->len;
    ram.blocks = (Block *)g_array_free(g_steal_pointer(&a), false);
}

static void chunk_unref(gpointer p)
{
    Chunk *c = p;

    if (!--c->refs) {
        qemu_anon_ram_free(c->data, chunk_map_size(c->size));
        g_free(c);
    }
}

XemuRamImage *xemu_ram_image_ref(XemuRamImage *img)
{
    img->refs++;
    return img;
}

void xemu_ram_image_unref(XemuRamImage *img)
{
    if (img && !--img->refs) {
        g_ptr_array_free(img->chunks, true);
        g_free(img->page);
        g_free(img);
    }
}

uint64_t xemu_ram_image_own_pages(XemuRamImage *img)
{
    return img->own;
}

size_t xemu_ram_image_page_size(void)
{
    blocks_init();
    return ram.page_size;
}

const uint8_t *const *xemu_ram_image_block(XemuRamImage *img, RAMBlock *rb,
                                           uint64_t *pages)
{
    uint8_t *host = qemu_ram_get_host_addr(rb);

    for (unsigned k = 0; k < ram.nblocks; k++) {
        if (ram.blocks[k].host == host) {
            *pages = ram.blocks[k].pages;
            return img->page + ram.blocks[k].first;
        }
    }
    return NULL;
}

static void set_synced(XemuRamImage *img)
{
    XemuRamImage *old = ram.synced;

    ram.synced = img ? xemu_ram_image_ref(img) : NULL;
    xemu_ram_image_unref(old);
}

/*
 * Move the log's bits into the blocks' bitmaps, clearing them (which arms
 * the store traps again), or with @clear false copy them.
 */
static void log_collect(bool clear)
{
    unsigned bits = qemu_target_page_bits();

    RCU_READ_LOCK_GUARD();
    DirtyMemoryBlocks *dm =
        qatomic_rcu_read(&ram_list.dirty_memory[DIRTY_MEMORY_MIGRATION]);

    for (unsigned k = 0; k < ram.nblocks; k++) {
        Block *b = &ram.blocks[k];
        uint64_t page0 = b->offset >> bits;
        bool any = false;

        for (uint64_t i = 0; i < b->pages;) {
            uint64_t p = page0 + i;
            unsigned long *map = dm->blocks[p / DIRTY_MEMORY_BLOCK_SIZE];
            uint64_t off = p % DIRTY_MEMORY_BLOCK_SIZE;

            if (!(off % BITS_PER_LONG) && !(i % BITS_PER_LONG) &&
                b->pages - i >= BITS_PER_LONG) {
                unsigned long *w = &map[off / BITS_PER_LONG];
                unsigned long v = qatomic_read(w);
                if (v) {
                    if (clear) {
                        v = qatomic_xchg(w, 0);
                    }
                    b->dirty[i / BITS_PER_LONG] |= v;
                    any = true;
                }
                i += BITS_PER_LONG;
            } else {
                if (test_bit(off, map) &&
                    (!clear || bitmap_test_and_clear_atomic(map, off, 1))) {
                    set_bit(i, b->dirty);
                    any = true;
                }
                i++;
            }
        }
        if (any && clear) {
            physical_memory_dirty_bits_cleared(b->offset,
                                               b->pages * ram.page_size);
        }
    }
}

/*
 * Migration is about to take the log's bits: keep a copy. Its syncs hold
 * the BQL, as everything else here does; the renderer's own dirty checks
 * (which sync too, from the PFIFO thread) don't take the bits.
 */
static void log_sync_global(MemoryListener *l, bool last_stage)
{
    if ((global_dirty_tracking & GLOBAL_DIRTY_MIGRATION) && bql_locked()) {
        log_collect(false);
    }
}

void xemu_ram_log_start(void)
{
    Error *err = NULL;

    if (ram.on) {
        return;
    }
    blocks_init();
    if (!memory_global_dirty_log_start(GLOBAL_DIRTY_XEMU, &err)) {
        error_report_err(err);
        return;
    }
    ram.listener = (MemoryListener) {
        .name = "xemu-checkpoint-ram",
        .log_sync_global = log_sync_global,
    };
    memory_listener_register(&ram.listener, &address_space_memory);
    ram.on = true;
    /* Writes before now weren't logged. */
    set_synced(NULL);
}

void xemu_ram_log_stop(void)
{
    /* No checkpoint is left to compare with: let RAM's image go too. */
    set_synced(NULL);
    if (!ram.on) {
        return;
    }
    memory_listener_unregister(&ram.listener);
    memory_global_dirty_log_stop(GLOBAL_DIRTY_XEMU);
    ram.on = false;
    /* As the log found them: all dirty, so no page traps for it. */
    for (unsigned k = 0; k < ram.nblocks; k++) {
        physical_memory_set_dirty_range(ram.blocks[k].offset,
                                        ram.blocks[k].pages * ram.page_size,
                                        1 << DIRTY_MEMORY_MIGRATION);
        bitmap_zero(ram.blocks[k].dirty, ram.blocks[k].pages);
    }
}

/*
 * The pages of @b that may differ from @img, in @cand: written since the
 * last save or restore, or held differently by @img and the image RAM
 * equalled then. False if any page may (no log, RAM unknown, a small
 * block).
 */
static bool block_candidates(Block *b, XemuRamImage *img,
                             unsigned long *cand)
{
    if (!ram.on || !ram.synced || !img || b->small) {
        return false;
    }
    bitmap_copy(cand, b->dirty, b->pages);
    if (img != ram.synced) {
        const uint8_t **x = ram.synced->page + b->first;
        const uint8_t **y = img->page + b->first;
        for (uint64_t i = 0; i < b->pages; i++) {
            if (x[i] != y[i]) {
                set_bit(i, cand);
            }
        }
    }
    return true;
}

/* The next page to look at: the next candidate, or every page. */
static uint64_t next_page(Block *b, unsigned long *cand, bool partial,
                          uint64_t i)
{
    return partial ? find_next_bit(cand, b->pages, i) : i;
}

/* RAM equals @img now (a save or a restore wrote it): restart the log. */
static void synced_to(XemuRamImage *img)
{
    for (unsigned k = 0; k < ram.nblocks; k++) {
        bitmap_zero(ram.blocks[k].dirty, ram.blocks[k].pages);
    }
    set_synced(img);
}

/*
 * A xemu_zero block (the Xbox's unused pc.ram, see RAMBlock) is all zero
 * and is never read: reading it would fault 128 MiB into the footprint.
 * Make @img hold it as zero pages; the number it held otherwise.
 */
static uint64_t zero_block_capture(XemuRamImage *img, Block *b)
{
    const uint8_t **p = img->page + b->first;
    uint64_t n = 0;

    for (uint64_t i = 0; i < b->pages; i++) {
        if (p[i]) {
            p[i] = NULL;
            n++;
        }
    }
    return n;
}

/* Whether @img holds the xemu_zero block @b as zero pages, as RAM does. */
static bool zero_block_matches(XemuRamImage *img, Block *b)
{
    const uint8_t **p = img->page + b->first;

    for (uint64_t i = 0; i < b->pages; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/*
 * Page @i of @b in a capture: false if it holds what the base does, else
 * a zero page or one to copy (in @copy).
 */
static bool capture_page(XemuRamImage *img, Block *b, uint64_t i,
                         bool has_base, unsigned long *copy, uint64_t *n)
{
    size_t ps = ram.page_size;
    const uint8_t *cur = b->host + i * ps;
    const uint8_t *old = img->page[b->first + i];

    if (has_base && (old ? !memcmp(cur, old, ps) : buffer_is_zero(cur, ps))) {
        return false;
    }
    if (buffer_is_zero(cur, ps)) {
        img->page[b->first + i] = NULL;
    } else {
        set_bit(b->first + i, copy);
        (*n)++;
    }
    return true;
}

XemuRamImage *xemu_ram_image_capture(XemuRamImage *base, bool verify,
                                     XemuRamStats *st)
{
    size_t ps;
    XemuRamImage *img = g_new0(XemuRamImage, 1);
    g_autofree unsigned long *cand = NULL;
    g_autofree unsigned long *copy = NULL;
    uint64_t n = 0;

    blocks_init();
    ps = ram.page_size;
    memset(st, 0, sizeof(*st));
    if (ram.on) {
        log_collect(true);
    }
    img->refs = 1;
    img->chunks = g_ptr_array_new_with_free_func(chunk_unref);
    if (base) {
        img->page = g_memdup2(base->page, ram.pages * sizeof(*img->page));
        for (guint i = 0; i < base->chunks->len; i++) {
            Chunk *c = g_ptr_array_index(base->chunks, i);
            c->refs++;
            g_ptr_array_add(img->chunks, c);
        }
    } else {
        img->page = g_new0(const uint8_t *, ram.pages);
    }

    /* Which pages to copy: those that differ from the base, or all. */
    copy = bitmap_new(ram.pages);
    for (unsigned k = 0; k < ram.nblocks; k++) {
        Block *b = &ram.blocks[k];
        bool partial;

        if (qemu_ram_xemu_zero(b->rb)) {
            st->differ += zero_block_capture(img, b);
            continue;
        }
        g_free(cand);
        cand = bitmap_new(b->pages);
        partial = block_candidates(b, base, cand);
        for (uint64_t i = next_page(b, cand, partial, 0); i < b->pages;
             i = next_page(b, cand, partial, i + 1)) {
            st->checked++;
            st->differ += capture_page(img, b, i, base, copy, &n);
        }
        if (partial && verify) {
            for (uint64_t i = find_first_zero_bit(cand, b->pages);
                 i < b->pages;
                 i = find_next_zero_bit(cand, b->pages, i + 1)) {
                st->missed += capture_page(img, b, i, base, copy, &n);
            }
        }
        st->logged |= partial;
    }

    if (n) {
        Chunk *c = g_new(Chunk, 1);
        uint8_t *dst;

        c->refs = 1;
        c->size = n * ps;
        c->data = dst = qemu_anon_ram_alloc(chunk_map_size(c->size), NULL,
                                            false, false);
        if (!dst) {
            error_report("xemu checkpoint: out of memory for %" PRIu64
                         " pages", n);
            abort();
        }
        g_ptr_array_add(img->chunks, c);
        for (unsigned k = 0; k < ram.nblocks; k++) {
            Block *b = &ram.blocks[k];
            for (uint64_t i = find_next_bit(copy, b->first + b->pages,
                                            b->first);
                 i < b->first + b->pages;
                 i = find_next_bit(copy, b->first + b->pages, i + 1)) {
                memcpy(dst, b->host + (i - b->first) * ps, ps);
                img->page[i] = dst;
                dst += ps;
            }
        }
    }
    img->own = n;
    synced_to(img);
    return img;
}

/* Write page @i of @b back from @img; true if it differed. */
static bool restore_page(Block *b, uint64_t i, XemuRamImage *img)
{
    size_t ps = ram.page_size;
    uint8_t *cur = b->host + i * ps;
    const uint8_t *want = img->page[b->first + i];

    if (want ? !memcmp(cur, want, ps) : buffer_is_zero(cur, ps)) {
        return false;
    }
    if (want) {
        memcpy(cur, want, ps);
    } else {
        memset(cur, 0, ps);
    }
    return true;
}

void xemu_ram_image_load(XemuRamImage *img, bool incremental, bool verify,
                         XemuRamStats *st)
{
    g_autofree unsigned long *cand = NULL;

    blocks_init();
    memset(st, 0, sizeof(*st));
    if (ram.on) {
        log_collect(true);
    }
    for (unsigned k = 0; k < ram.nblocks; k++) {
        Block *b = &ram.blocks[k];
        bool partial;

        if (qemu_ram_xemu_zero(b->rb)) {
            if (zero_block_matches(img, b)) {
                continue;
            }
            /* The image holds data for it: write it back as usual. */
            b->rb->xemu_zero = false;
        }
        g_free(cand);
        cand = bitmap_new(b->pages);
        partial = incremental && block_candidates(b, img, cand);
        for (uint64_t i = next_page(b, cand, partial, 0); i < b->pages;
             i = next_page(b, cand, partial, i + 1)) {
            st->checked++;
            st->differ += restore_page(b, i, img);
        }
        if (partial && verify) {
            for (uint64_t i = find_first_zero_bit(cand, b->pages);
                 i < b->pages;
                 i = find_next_zero_bit(cand, b->pages, i + 1)) {
                st->missed += restore_page(b, i, img);
            }
        }
        st->logged |= partial;
    }
    synced_to(img);
}

void xemu_ram_restore_begin(void)
{
    ram.restoring = true;
}

void xemu_ram_restore_end(bool ok)
{
    ram.restoring = false;
    if (!ok) {
        set_synced(NULL);
    }
}

/* Every qemu_loadvm_state: one that isn't a checkpoint's replaces RAM. */
void xemu_checkpoint_ram_loading(void)
{
    if (!ram.restoring) {
        set_synced(NULL);
    }
}
