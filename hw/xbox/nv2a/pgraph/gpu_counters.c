/*
 * NV2A workload counters (xemu-macos 0.16.1)
 *
 * QMP xemu-gpu-counters-start / -stop / query-xemu-gpu-counters and
 * xemu-frame-advance's gpu-counters: begin/end pairs, vertices by path,
 * primitives by type, pushbuffer, vertex, texture, surface, clear and
 * report bytes, below and above 64 MiB physical. Counted in PGRAPH's
 * method handlers (pgraph.c, vertex.c) and the PFIFO pusher (pfifo.c),
 * never in a renderer, so the null and Vulkan renderers count the same
 * work for the same command stream; only occlusion-samples comes from the
 * renderer (the values it writes into reports).
 *
 * Off (the default), every hook site costs one predicted branch on
 * pg->gc_on. On, the hooks read PGRAPH state and guest RAM (texture
 * content hashes) and write only this file's state: no guest RAM,
 * register, timer or IRQ changes, so RAM digests and deterministic replays
 * are the same with counting on and off.
 *
 * Threads: the hooks run on the one thread that runs PGRAPH methods at a
 * time (the PFIFO thread, or under icount the inline pusher's thread, with
 * both FIFO locks), which owns cur[] and the per-flip state; QMP (main
 * loop, BQL) starts, reads and stops. A start bumps `session`; the method
 * thread resets its own state when it next sees a new session, so QMP never
 * frees or clears what the method thread is using; the state itself is
 * allocated once, at PGRAPH init (nv2a_gc_init). Under icount, with the VM
 * paused, the pusher is at rest and the counts are exact.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "texture.h"
#include "gpu_counters.h"

#define GC_HIGH 0x04000000ULL      /* 64 MiB */
#define GC_PER_FLIP_MAX 65536
#define GC_TEX_SLOTS 4096          /* distinct textures per flip (power of 2) */
#define GC_SURF_SLOTS 256          /* distinct render targets per flip */
#define GC_TEX_HASHES_MAX 65536    /* content hashes kept per counting run */

#define GC_FIELDS(X) \
    X(BEGIN_END, begin_end) \
    X(VERTICES, vertices) \
    X(VTX_DA, vertices_draw_arrays) \
    X(VTX_IA, vertices_inline_array) \
    X(VTX_AE16, vertices_array_element16) \
    X(VTX_AE32, vertices_array_element32) \
    X(VTX_IMM, vertices_immediate) \
    X(PRIM_POINTS, prim_points) \
    X(PRIM_LINES, prim_lines) \
    X(PRIM_LINE_LOOP, prim_line_loop) \
    X(PRIM_LINE_STRIP, prim_line_strip) \
    X(PRIM_TRIANGLES, prim_triangles) \
    X(PRIM_TRIANGLE_STRIP, prim_triangle_strip) \
    X(PRIM_TRIANGLE_FAN, prim_triangle_fan) \
    X(PRIM_QUADS, prim_quads) \
    X(PRIM_QUAD_STRIP, prim_quad_strip) \
    X(PRIM_POLYGON, prim_polygon) \
    X(TRIANGLES, triangles) \
    X(PB_BYTES, pushbuffer_bytes) \
    X(PB_HIGH, pushbuffer_bytes_high) \
    X(VTX_BYTES, vertex_bytes) \
    X(VTX_HIGH, vertex_bytes_high) \
    X(TEX_BINDS, texture_binds) \
    X(TEX_BOUND, texture_bytes_bound) \
    X(TEX_BYTES, texture_bytes) \
    X(TEX_HIGH, texture_bytes_high) \
    X(TEX_UPLOADED, texture_bytes_uploaded) \
    X(TEX_NEW, texture_bytes_new) \
    X(SURF_BYTES, surface_bytes) \
    X(SURF_HIGH, surface_bytes_high) \
    X(CLEARS, clears) \
    X(CLEAR_PIXELS, clear_pixels) \
    X(CLEAR_BYTES, clear_bytes) \
    X(CLEAR_HIGH, clear_bytes_high) \
    X(REPORTS, reports) \
    X(REPORT_BYTES, report_bytes) \
    X(REPORT_HIGH, report_bytes_high) \
    X(BLITS, blits) \
    X(BLIT_PIXELS, blit_pixels) \
    X(READ_LOW, read_bytes_low) \
    X(READ_HIGH, read_bytes_high) \
    X(WRITE_LOW, write_bytes_low) \
    X(WRITE_HIGH, write_bytes_high) \
    X(OCCLUSION, occlusion_samples)

enum {
#define X(e, f) GC_##e,
    GC_FIELDS(X)
#undef X
    GC_N
};

typedef struct GCSeen {
    uint64_t key;
    uint32_t gen;
} GCSeen;

typedef struct GCAttr {
    hwaddr base;
    uint32_t stride;
    uint32_t size;
} GCAttr;

typedef struct NV2AGpuCounters {
    /* QMP side, under lock (the flip fold takes it too) */
    QemuMutex lock;
    uint64_t total[GC_N];
    uint64_t max[GC_N];
    uint64_t flips;
    bool per_flip;
    GArray *rows;              /* uint64_t[GC_N] per flip */
    uint64_t rows_dropped;
    unsigned int session;      /* bumped by each start (qatomic) */
    uint64_t occlusion;        /* qatomic: report deliveries, any thread */

    /* Method thread only (the pusher's thread at a time) */
    unsigned int seen_session;
    uint64_t cur[GC_N];        /* this flip; QMP reads it with qatomic_read */
    uint64_t pair_vtx;         /* vertices of the open begin/end pair */
    uint64_t pair_ia_words;    /* its INLINE_ARRAY words */
    uint32_t ae_min, ae_max;   /* its array element index range */
    bool ae_any;
    bool prof_valid;           /* prof[] holds the pair's array attributes */
    int prof_n;
    GCAttr prof[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint32_t gen;              /* per-flip generation for the seen sets */
    GCSeen tex_seen[GC_TEX_SLOTS];
    GCSeen surf_seen[GC_SURF_SLOTS];
    GHashTable *tex_hash;      /* texture key -> content hash, this run */
} NV2AGpuCounters;

static NV2AGpuCounters *gc_state;
static bool gc_frame_advance;  /* a frame advance owns the counting run */

#define GC_ADD(gc, i, n) \
    qatomic_set(&(gc)->cur[i], (gc)->cur[i] + (uint64_t)(n))

static inline NV2AState *gc_d(PGRAPHState *pg)
{
    return container_of(pg, NV2AState, pgraph);
}

/* The method thread's view, its state reset at a new session. */
static NV2AGpuCounters *gc_get(void)
{
    NV2AGpuCounters *gc = gc_state;
    unsigned int s = qatomic_read(&gc->session);
    if (unlikely(gc->seen_session != s)) {
        memset(gc->cur, 0, sizeof(gc->cur));
        gc->pair_vtx = gc->pair_ia_words = 0;
        gc->ae_any = false;
        gc->prof_valid = false;
        gc->gen++;
        g_hash_table_remove_all(gc->tex_hash);
        /* Publishes the clear before QMP trusts cur[] for this session. */
        qatomic_store_release(&gc->seen_session, s);
    }
    return gc;
}

/* The bytes of [addr, addr + len) at or above 64 MiB. */
static inline uint64_t gc_high_part(uint64_t addr, uint64_t len)
{
    if (addr + len <= GC_HIGH) {
        return 0;
    }
    return addr >= GC_HIGH ? len : addr + len - GC_HIGH;
}

/* len bytes at addr in the read or write split only. */
static uint64_t gc_rw(NV2AGpuCounters *gc, bool write, uint64_t addr,
                      uint64_t len)
{
    uint64_t hi = gc_high_part(addr, len);
    GC_ADD(gc, write ? GC_WRITE_LOW : GC_READ_LOW, len - hi);
    GC_ADD(gc, write ? GC_WRITE_HIGH : GC_READ_HIGH, hi);
    return hi;
}

/* len bytes at addr: counter tot (and high), and the read/write split. */
static void gc_span(NV2AGpuCounters *gc, int tot, int high, bool write,
                    uint64_t addr, uint64_t len)
{
    uint64_t hi = gc_rw(gc, write, addr, len);
    GC_ADD(gc, tot, len);
    GC_ADD(gc, high, hi);
}

/* A DMA object's base (as nv_dma_map masks it), or false if unusable. */
static bool gc_dma_base(NV2AState *d, hwaddr obj, hwaddr *base)
{
    if (obj >= memory_region_size(&d->ramin)) {
        return false;
    }
    DMAObject dma = nv_dma_load(d, obj);
    *base = dma.address & 0x07FFFFFF;
    return true;
}

/* True the first time key is seen in this flip. */
static bool gc_first_in_flip(NV2AGpuCounters *gc, GCSeen *set, int slots,
                             uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    for (int probe = 0; probe < slots; probe++) {
        GCSeen *e = &set[(h + probe) & (slots - 1)];
        if (e->gen != gc->gen) {
            e->gen = gc->gen;
            e->key = key;
            return true;
        }
        if (e->key == key) {
            return false;
        }
    }
    return true; /* full: count it again rather than lose it */
}

static uint64_t gc_mix(uint64_t a, uint64_t b)
{
    uint64_t v[2] = { a, b };
    return fast_hash((const uint8_t *)v, sizeof(v));
}

/* ---- pushbuffer ---------------------------------------------------------- */

void nv2a_gc_pushbuf(NV2AState *d, hwaddr base, uint32_t old_get,
                     uint32_t new_get, uint32_t word, uint32_t method_count)
{
    NV2AGpuCounters *gc = gc_get();
    uint64_t words = 1;

    /* A jump, call or return moves GET elsewhere after its one word. */
    bool branch = !method_count &&
                  ((word & 0xe0000003) == 0x20000000 || (word & 3) == 1 ||
                   (word & 3) == 2 || word == 0x00020000);
    if (!branch && new_get > old_get) {
        words = (new_get - old_get) / 4;
    }
    gc_span(gc, GC_PB_BYTES, GC_PB_HIGH, false, base + old_get, words * 4);
}

/* ---- begin/end pairs ----------------------------------------------------- */

static void gc_pair_reset(NV2AGpuCounters *gc)
{
    gc->pair_vtx = 0;
    gc->pair_ia_words = 0;
    gc->ae_any = false;
    gc->prof_valid = false;
}

void nv2a_gc_begin(PGRAPHState *pg)
{
    gc_pair_reset(gc_get());
}

/* The pair's array attributes: base, stride and element size. */
static void gc_profile(PGRAPHState *pg, NV2AGpuCounters *gc)
{
    NV2AState *d = gc_d(pg);
    hwaddr base_a, base_b;
    bool ok_a = gc_dma_base(d, pg->dma_vertex_a, &base_a);
    bool ok_b = gc_dma_base(d, pg->dma_vertex_b, &base_b);

    gc->prof_n = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!attr->count || !(attr->dma_select ? ok_b : ok_a)) {
            continue;
        }
        gc->prof[gc->prof_n++] = (GCAttr){
            .base = (attr->dma_select ? base_b : base_a) + attr->offset,
            .stride = attr->stride,
            .size = attr->size * attr->count,
        };
    }
    gc->prof_valid = true;
}

/* Attribute reads of vertices first..last (inclusive) of the pair's arrays */
static void gc_fetch(PGRAPHState *pg, NV2AGpuCounters *gc, uint32_t first,
                     uint32_t last)
{
    if (!gc->prof_valid) {
        gc_profile(pg, gc);
    }
    for (int i = 0; i < gc->prof_n; i++) {
        GCAttr *a = &gc->prof[i];
        uint64_t len = (uint64_t)a->stride * (last - first) + a->size;
        gc_span(gc, GC_VTX_BYTES, GC_VTX_HIGH, false,
                a->base + (uint64_t)a->stride * first, len);
    }
}

void nv2a_gc_draw_arrays(PGRAPHState *pg, uint32_t start, uint32_t count)
{
    NV2AGpuCounters *gc = gc_get();
    GC_ADD(gc, GC_VTX_DA, count);
    GC_ADD(gc, GC_VERTICES, count);
    gc->pair_vtx += count;
    if (count) {
        gc_fetch(pg, gc, start, start + count - 1);
    }
}

void nv2a_gc_array_element(PGRAPHState *pg, uint32_t parameter, bool pair16)
{
    NV2AGpuCounters *gc = gc_get();
    uint32_t lo, hi;
    if (pair16) {
        uint32_t a = parameter & 0xffff, b = parameter >> 16;
        lo = MIN(a, b);
        hi = MAX(a, b);
        GC_ADD(gc, GC_VTX_AE16, 2);
        GC_ADD(gc, GC_VERTICES, 2);
        gc->pair_vtx += 2;
    } else {
        lo = hi = parameter;
        GC_ADD(gc, GC_VTX_AE32, 1);
        GC_ADD(gc, GC_VERTICES, 1);
        gc->pair_vtx += 1;
    }
    if (!gc->ae_any) {
        gc->ae_min = lo;
        gc->ae_max = hi;
        gc->ae_any = true;
    } else {
        gc->ae_min = MIN(gc->ae_min, lo);
        gc->ae_max = MAX(gc->ae_max, hi);
    }
}

void nv2a_gc_inline_array_word(PGRAPHState *pg)
{
    gc_get()->pair_ia_words++;
}

void nv2a_gc_immediate_vertex(PGRAPHState *pg)
{
    NV2AGpuCounters *gc = gc_get();
    GC_ADD(gc, GC_VTX_IMM, 1);
    GC_ADD(gc, GC_VERTICES, 1);
    gc->pair_vtx++;
}

/* The inline array vertex size, laid out as the renderers do. */
static unsigned int gc_inline_vertex_size(PGRAPHState *pg)
{
    unsigned int offset = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!attr->count || !attr->size) {
            continue;
        }
        offset = ROUND_UP(offset, attr->size);
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }
    return offset;
}

static void gc_primitives(NV2AGpuCounters *gc, uint32_t mode, uint64_t n)
{
    uint64_t p = 0, tris = 0;
    int idx = -1;

    switch (mode) {
    case NV097_SET_BEGIN_END_OP_POINTS:
        idx = GC_PRIM_POINTS; p = n;
        break;
    case NV097_SET_BEGIN_END_OP_LINES:
        idx = GC_PRIM_LINES; p = n / 2;
        break;
    case NV097_SET_BEGIN_END_OP_LINE_LOOP:
        idx = GC_PRIM_LINE_LOOP; p = n >= 2 ? n : 0;
        break;
    case NV097_SET_BEGIN_END_OP_LINE_STRIP:
        idx = GC_PRIM_LINE_STRIP; p = n >= 2 ? n - 1 : 0;
        break;
    case NV097_SET_BEGIN_END_OP_TRIANGLES:
        idx = GC_PRIM_TRIANGLES; p = tris = n / 3;
        break;
    case NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP:
        idx = GC_PRIM_TRIANGLE_STRIP; p = tris = n >= 3 ? n - 2 : 0;
        break;
    case NV097_SET_BEGIN_END_OP_TRIANGLE_FAN:
        idx = GC_PRIM_TRIANGLE_FAN; p = tris = n >= 3 ? n - 2 : 0;
        break;
    case NV097_SET_BEGIN_END_OP_QUADS:
        idx = GC_PRIM_QUADS; p = n / 4; tris = 2 * p;
        break;
    case NV097_SET_BEGIN_END_OP_QUAD_STRIP:
        idx = GC_PRIM_QUAD_STRIP; p = n >= 4 ? (n - 2) / 2 : 0; tris = 2 * p;
        break;
    case NV097_SET_BEGIN_END_OP_POLYGON:
        idx = GC_PRIM_POLYGON; p = n >= 3; tris = n >= 3 ? n - 2 : 0;
        break;
    default:
        return;
    }
    GC_ADD(gc, idx, p);
    GC_ADD(gc, GC_TRIANGLES, tris);
}

/* Texture size as the renderers compute it, without their asserts. */
static uint64_t gc_texture_length(PGRAPHState *pg, int i, unsigned int *fmt_out)
{
    uint32_t ctl_0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
    uint32_t ctl_1 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i * 4);
    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
    uint32_t rect = pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i * 4);
    unsigned int color_format = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_COLOR);
    *fmt_out = color_format;
    if (color_format >= ARRAY_SIZE(kelvin_color_format_info_map)) {
        return 0;
    }
    BasicColorFormatInfo f = kelvin_color_format_info_map[color_format];
    if (!f.bytes_per_pixel) {
        return 0;
    }
    bool cubemap = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
    unsigned int dim = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
    if (f.linear) {
        return (uint64_t)GET_MASK(rect, NV_PGRAPH_TEXIMAGERECT0_HEIGHT) *
               GET_MASK(ctl_1, NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH);
    }
    unsigned int lw = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
    unsigned int lh = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
    unsigned int ld = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);
    unsigned int levels = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
    unsigned int max_lod = GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MAX_LOD_CLAMP);
    levels = MIN(levels, max_lod + 1);
    levels = MIN(levels, MAX(lw, lh) + 1);
    if (dim == 3) {
        levels = (lw < 2 || lh < 2) ? 1 : MIN(levels, MIN(lw, lh) - 1);
    }
    levels = MAX(levels, 1);
    if (dim < 2) {
        return 0;
    }
    bool dxt = pgraph_is_texture_format_compressed(pg, color_format);
    unsigned int block = color_format ==
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 : 16;
    uint64_t w = 1ULL << lw, h = 1ULL << lh, length = 0;
    for (unsigned int l = 0; l < levels; l++) {
        w = MAX(w, 1);
        h = MAX(h, 1);
        length += dxt ? ((w + 3) / 4) * ((h + 3) / 4) * block
                      : w * h * f.bytes_per_pixel;
        w /= 2;
        h /= 2;
    }
    if (cubemap) {
        length = ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT) * 6;
    }
    if (dim >= 3) {
        length <<= ld;
    }
    return length;
}

static void gc_textures(PGRAPHState *pg, NV2AGpuCounters *gc)
{
    NV2AState *d = gc_d(pg);
    uint64_t ram_size = memory_region_size(d->vram);
    hwaddr base_a, base_b;
    bool ok_a = gc_dma_base(d, pg->dma_a, &base_a);
    bool ok_b = gc_dma_base(d, pg->dma_b, &base_b);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            continue;
        }
        unsigned int color_format;
        uint64_t length = gc_texture_length(pg, i, &color_format);
        uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        bool dma_b = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);
        if (!length || !(dma_b ? ok_b : ok_a)) {
            continue;
        }
        uint64_t addr = (dma_b ? base_b : base_a) +
                        pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4);

        uint64_t pal_addr = 0, pal_len = 0;
        if (color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            uint32_t pal = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + i * 4);
            bool pal_b = GET_MASK(pal, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA);
            static const unsigned int pal_entries[] = { 256, 128, 64, 32 };
            unsigned int li = GET_MASK(pal, NV_PGRAPH_TEXPALETTE0_LENGTH);
            if ((pal_b ? ok_b : ok_a) && li < ARRAY_SIZE(pal_entries)) {
                pal_addr = (pal_b ? base_b : base_a) +
                           (pal & NV_PGRAPH_TEXPALETTE0_OFFSET);
                pal_len = pal_entries[li] * 4;
            }
        }

        GC_ADD(gc, GC_TEX_BINDS, 1);
        GC_ADD(gc, GC_TEX_BOUND, length + pal_len);

        uint64_t key = gc_mix(addr ^ (length << 32) ^ color_format,
                              pal_addr ^ (pal_len << 48));
        if (!gc_first_in_flip(gc, gc->tex_seen, GC_TEX_SLOTS, key)) {
            continue;
        }
        gc_span(gc, GC_TEX_BYTES, GC_TEX_HIGH, false, addr, length);
        if (pal_len) {
            gc_span(gc, GC_TEX_BYTES, GC_TEX_HIGH, false, pal_addr, pal_len);
        }

        /* Content: a renderer must upload a texture whose bytes changed. */
        uint64_t content = 0;
        if (addr < ram_size) {
            content = fast_hash(d->vram_ptr + addr,
                                MIN(length, ram_size - addr));
        }
        if (pal_len && pal_addr < ram_size) {
            content ^= gc_mix(content,
                              fast_hash(d->vram_ptr + pal_addr,
                                        MIN(pal_len, ram_size - pal_addr)));
        }
        gpointer old;
        if (!g_hash_table_lookup_extended(gc->tex_hash, &key, NULL, &old)) {
            GC_ADD(gc, GC_TEX_NEW, length + pal_len);
            if (g_hash_table_size(gc->tex_hash) >= GC_TEX_HASHES_MAX) {
                g_hash_table_remove_all(gc->tex_hash);
            }
            uint64_t *k = g_new(uint64_t, 1), *v = g_new(uint64_t, 1);
            *k = key;
            *v = content;
            g_hash_table_insert(gc->tex_hash, k, v);
        } else if (*(uint64_t *)old != content) {
            GC_ADD(gc, GC_TEX_UPLOADED, length + pal_len);
            *(uint64_t *)old = content;
        }
    }
}

static unsigned int gc_color_bpp(unsigned int f)
{
    switch (f) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return 1;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return 2;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return 4;
    default:
        return 0;
    }
}

static unsigned int gc_zeta_bpp(unsigned int f)
{
    return f == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 :
           f == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ? 4 : 0;
}

/* The bound surface's size in pixels (before anti-aliasing), as bound. */
static void gc_surface_dims(PGRAPHState *pg, unsigned int *w, unsigned int *h)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        *w = 1u << pg->surface_shape.log_width;
        *h = 1u << pg->surface_shape.log_height;
    } else {
        *w = pg->surface_shape.clip_x + pg->surface_shape.clip_width;
        *h = pg->surface_shape.clip_y + pg->surface_shape.clip_height;
    }
}

static void gc_aa_factor(PGRAPHState *pg, unsigned int *fx, unsigned int *fy)
{
    *fx = *fy = 1;
    switch (pg->surface_shape.anti_aliasing) {
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2:
        *fx = 2;
        break;
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4:
        *fx = *fy = 2;
        break;
    default:
        break;
    }
}

/* A surface's address, or false when its DMA object can't be read. */
static bool gc_surface_addr(PGRAPHState *pg, bool color, uint64_t *addr)
{
    hwaddr base;
    if (!gc_dma_base(gc_d(pg), color ? pg->dma_color : pg->dma_zeta, &base)) {
        return false;
    }
    *addr = base + (color ? pg->surface_color.offset : pg->surface_zeta.offset);
    return true;
}

static void gc_surface(PGRAPHState *pg, NV2AGpuCounters *gc, bool color)
{
    unsigned int bpp = color ? gc_color_bpp(pg->surface_shape.color_format)
                             : gc_zeta_bpp(pg->surface_shape.zeta_format);
    uint64_t addr;
    if (!bpp || !gc_surface_addr(pg, color, &addr)) {
        return;
    }
    unsigned int w, h, fx, fy;
    gc_surface_dims(pg, &w, &h);
    gc_aa_factor(pg, &fx, &fy);
    w *= fx;
    h *= fy;
    unsigned int pitch = color ? pg->surface_color.pitch : pg->surface_zeta.pitch;
    uint64_t size = (uint64_t)h * MAX(pitch, w * bpp);
    if (!size) {
        return;
    }
    uint64_t key = gc_mix(addr ^ ((uint64_t)color << 63), size);
    if (gc_first_in_flip(gc, gc->surf_seen, GC_SURF_SLOTS, key)) {
        gc_span(gc, GC_SURF_BYTES, GC_SURF_HIGH, true, addr, size);
    }
}

/* A pair's accounting, at END (or the pusher's squash of END, BEGIN). */
void nv2a_gc_end(PGRAPHState *pg)
{
    NV2AGpuCounters *gc = gc_get();

    if (gc->pair_ia_words) {
        unsigned int vsize = gc_inline_vertex_size(pg);
        uint64_t n = vsize ? gc->pair_ia_words * 4 / vsize : 0;
        GC_ADD(gc, GC_VTX_IA, n);
        GC_ADD(gc, GC_VERTICES, n);
        gc->pair_vtx += n;
    }
    if (gc->ae_any) {
        gc_fetch(pg, gc, gc->ae_min, gc->ae_max);
    }
    GC_ADD(gc, GC_BEGIN_END, 1);
    gc_primitives(gc, pg->primitive_mode, gc->pair_vtx);
    gc_textures(pg, gc);
    if (pgraph_color_write_enabled(pg)) {
        gc_surface(pg, gc, true);
    }
    if (pgraph_zeta_write_enabled(pg)) {
        gc_surface(pg, gc, false);
    }
    gc_pair_reset(gc);
}

/* ---- clears, reports, blits ---------------------------------------------- */

void nv2a_gc_clear(PGRAPHState *pg, uint32_t parameter)
{
    NV2AGpuCounters *gc = gc_get();
    uint32_t rx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t ry = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
    unsigned int xmin = GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMAX);
    unsigned int w, h, fx, fy;

    GC_ADD(gc, GC_CLEARS, 1);
    gc_surface_dims(pg, &w, &h);
    if (!w || !h) {
        return;
    }
    xmin = MIN(xmin, w - 1);
    ymin = MIN(ymin, h - 1);
    xmax = MAX(xmin, MIN(xmax, w - 1));
    ymax = MAX(ymin, MIN(ymax, h - 1));
    gc_aa_factor(pg, &fx, &fy);
    uint64_t pixels = (uint64_t)(xmax - xmin + 1) * fx * (ymax - ymin + 1) * fy;
    GC_ADD(gc, GC_CLEAR_PIXELS, pixels);

    for (int color = 1; color >= 0; color--) {
        bool cleared = color ? (parameter & NV097_CLEAR_SURFACE_COLOR)
                             : (parameter & (NV097_CLEAR_SURFACE_Z |
                                             NV097_CLEAR_SURFACE_STENCIL));
        unsigned int bpp = color ? gc_color_bpp(pg->surface_shape.color_format)
                                 : gc_zeta_bpp(pg->surface_shape.zeta_format);
        uint64_t addr;
        if (!cleared || !bpp || !gc_surface_addr(pg, color, &addr)) {
            continue;
        }
        unsigned int pitch = color ? pg->surface_color.pitch
                                   : pg->surface_zeta.pitch;
        gc_span(gc, GC_CLEAR_BYTES, GC_CLEAR_HIGH, true,
                addr + (uint64_t)ymin * fy * pitch, pixels * bpp);
    }
}

void nv2a_gc_report(PGRAPHState *pg, hwaddr addr)
{
    NV2AGpuCounters *gc = gc_get();
    GC_ADD(gc, GC_REPORTS, 1);
    gc_span(gc, GC_REPORT_BYTES, GC_REPORT_HIGH, true, addr, 16);
}

void nv2a_gc_semaphore(PGRAPHState *pg, hwaddr addr)
{
    gc_span(gc_get(), GC_REPORT_BYTES, GC_REPORT_HIGH, true, addr, 4);
}

void nv2a_gc_blit(PGRAPHState *pg)
{
    NV2AGpuCounters *gc = gc_get();
    ContextSurfaces2DState *cs = &pg->context_surfaces_2d;
    ImageBlitState *ib = &pg->image_blit;
    NV2AState *d = gc_d(pg);
    unsigned int bpp;

    GC_ADD(gc, GC_BLITS, 1);
    GC_ADD(gc, GC_BLIT_PIXELS, (uint64_t)ib->width * ib->height);
    switch (cs->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        bpp = 1;
        break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        bpp = 2;
        break;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        bpp = 4;
        break;
    default:
        return;
    }
    hwaddr src, dst;
    uint64_t bytes = (uint64_t)ib->width * ib->height * bpp;
    if (gc_dma_base(d, cs->dma_image_source, &src)) {
        gc_rw(gc, false, src + cs->source_offset +
              (uint64_t)ib->in_y * cs->source_pitch + ib->in_x * bpp, bytes);
    }
    if (gc_dma_base(d, cs->dma_image_dest, &dst)) {
        gc_rw(gc, true, dst + cs->dest_offset +
              (uint64_t)ib->out_y * cs->dest_pitch + ib->out_x * bpp, bytes);
    }
}

void nv2a_gc_occlusion(PGRAPHState *pg, uint32_t samples)
{
    qatomic_add(&gc_state->occlusion, samples);
}

/* ---- flips and QMP ------------------------------------------------------- */

void nv2a_gc_flip(PGRAPHState *pg)
{
    NV2AGpuCounters *gc = gc_get();

    qemu_mutex_lock(&gc->lock);
    if (gc->seen_session == qatomic_read(&gc->session)) {
        gc->cur[GC_OCCLUSION] += qatomic_xchg(&gc->occlusion, 0);
        for (int i = 0; i < GC_N; i++) {
            gc->total[i] += gc->cur[i];
            gc->max[i] = MAX(gc->max[i], gc->cur[i]);
        }
        gc->flips++;
        if (gc->per_flip) {
            if (gc->rows->len < GC_PER_FLIP_MAX) {
                g_array_append_vals(gc->rows, gc->cur, GC_N);
            } else {
                gc->rows_dropped++;
            }
        }
    }
    for (int i = 0; i < GC_N; i++) {
        qatomic_set(&gc->cur[i], 0);
    }
    gc->gen++;
    qemu_mutex_unlock(&gc->lock);
}

static XemuGpuCounterSet *gc_set(const uint64_t *v)
{
    XemuGpuCounterSet *s = g_new0(XemuGpuCounterSet, 1);
#define X(e, f) s->f = v[GC_##e];
    GC_FIELDS(X)
#undef X
    return s;
}

static bool gc_running(void)
{
    return gc_state && qatomic_read(&g_nv2a->pgraph.gc_on);
}

/* The counts so far (gc->lock held). */
static XemuGpuCounters *gc_result(NV2AGpuCounters *gc, bool rows)
{
    uint64_t tail[GC_N] = { 0 }, total[GC_N];
    XemuGpuCounters *r = g_new0(XemuGpuCounters, 1);

    /* cur[] belongs to this session once the method thread reset it. */
    if (qatomic_load_acquire(&gc->seen_session) ==
        qatomic_read(&gc->session)) {
        for (int i = 0; i < GC_N; i++) {
            tail[i] = qatomic_read(&gc->cur[i]);
        }
    }
    tail[GC_OCCLUSION] += qatomic_read(&gc->occlusion);
    for (int i = 0; i < GC_N; i++) {
        total[i] = gc->total[i] + tail[i];
    }
    r->flips = gc->flips;
    r->total = gc_set(total);
    r->max = gc_set(gc->max);
    r->tail = gc_set(tail);
    if (rows && gc->per_flip) {
        XemuGpuCounterSetList **t = &r->per_flip;
        const uint64_t *v = (const uint64_t *)gc->rows->data;
        for (guint i = 0; i < gc->rows->len / GC_N; i++) {
            QAPI_LIST_APPEND(t, gc_set(v + (size_t)i * GC_N));
        }
        r->has_per_flip = true;
        r->has_per_flip_dropped = true;
        r->per_flip_dropped = gc->rows_dropped;
    }
    return r;
}

/*
 * At PGRAPH init, before the PFIFO thread exists: the hooks read gc_state
 * (and tex_hash) after a plain read of pg->gc_on, which orders nothing
 * against the QMP thread's stores, so the state is never published later.
 */
void nv2a_gc_init(void)
{
    NV2AGpuCounters *gc = g_new0(NV2AGpuCounters, 1);
    qemu_mutex_init(&gc->lock);
    gc->rows = g_array_new(false, false, sizeof(uint64_t));
    gc->tex_hash = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, g_free);
    gc_state = gc;
}

static bool gc_start(bool per_flip, Error **errp)
{
    if (!g_nv2a || !gc_state) {
        error_setg(errp, "no NV2A");
        return false;
    }
    if (gc_running()) {
        error_setg(errp, "GPU counters are already running");
        return false;
    }
    NV2AGpuCounters *gc = gc_state;
    qemu_mutex_lock(&gc->lock);
    memset(gc->total, 0, sizeof(gc->total));
    memset(gc->max, 0, sizeof(gc->max));
    gc->flips = 0;
    gc->per_flip = per_flip;
    g_array_set_size(gc->rows, 0);
    gc->rows_dropped = 0;
    qatomic_set(&gc->occlusion, 0);
    qatomic_inc(&gc->session);
    qemu_mutex_unlock(&gc->lock);
    /*
     * The flag after the session. The hooks' plain flag read doesn't order
     * their session read, so a hook may still count one stretch for the old
     * session: its next look at the session drops it (gc_get), and neither
     * QMP nor the flip fold trusts cur[] of a session not yet taken up.
     */
    smp_wmb();
    qatomic_set(&g_nv2a->pgraph.gc_on, true);
    return true;
}

static XemuGpuCounters *gc_stop(void)
{
    NV2AGpuCounters *gc = gc_state;
    qatomic_set(&g_nv2a->pgraph.gc_on, false);
    qemu_mutex_lock(&gc->lock);
    XemuGpuCounters *r = gc_result(gc, true);
    g_array_set_size(gc->rows, 0);
    qemu_mutex_unlock(&gc->lock);
    return r;
}

void qmp_xemu_gpu_counters_start(bool has_per_flip, bool per_flip,
                                 Error **errp)
{
    gc_start(has_per_flip && per_flip, errp);
}

XemuGpuCounters *qmp_xemu_gpu_counters_stop(Error **errp)
{
    if (!gc_running() || gc_frame_advance) {
        error_setg(errp, gc_frame_advance ?
                   "GPU counters belong to the running frame advance" :
                   "GPU counters are not running");
        return NULL;
    }
    return gc_stop();
}

XemuGpuCounters *qmp_query_xemu_gpu_counters(Error **errp)
{
    if (!gc_running()) {
        error_setg(errp, "GPU counters are not running");
        return NULL;
    }
    qemu_mutex_lock(&gc_state->lock);
    XemuGpuCounters *r = gc_result(gc_state, true);
    qemu_mutex_unlock(&gc_state->lock);
    return r;
}

/* xemu-frame-advance's gpu-counters (ui/xemu.c), BQL held. */
bool nv2a_gpu_counters_advance_begin(bool per_flip, Error **errp)
{
    if (!gc_start(per_flip, errp)) {
        return false;
    }
    gc_frame_advance = true;
    return true;
}

XemuGpuCounters *nv2a_gpu_counters_advance_end(void)
{
    gc_frame_advance = false;
    return gc_stop();
}
