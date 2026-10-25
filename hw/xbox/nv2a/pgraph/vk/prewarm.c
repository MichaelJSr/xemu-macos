/*
 * Geforce NV2A PGRAPH Vulkan Renderer: pipeline prewarm
 *
 * Each draw pipeline the renderer builds on a real miss has its key
 * recorded in a manifest beside the pipeline cache, one per driver and key
 * schema (<base>vk_prewarm-<driver>-<schema>.bin). At the next launch a
 * background thread rebuilds the recorded pipelines, newest first, and
 * destroys each at once: what it keeps is the drivers' caches (MoltenVK's
 * shader libraries, Metal's compiled functions, the SPIR-V and pipeline
 * caches on disk). When a scene then asks for one of them, the renderer's
 * own creation is a warm one, about a millisecond, instead of a compile of
 * 30-90 ms: an effect's first use (Azurik's fire armor: ~160 pipelines)
 * took seconds with empty caches, as after every update.
 *
 * The worker builds from the key alone, through the renderer's own builder
 * (pgraph_vk_build_draw_pipeline) and module path, so it makes exactly
 * what the renderer would, and nothing it makes ever draws: pictures and
 * guest state cannot change. It touches only the device, the shared
 * VkPipelineCache (internally synchronized), init-time renderer fields and
 * objects of its own; never PGRAPH state or the renderer's caches, and no
 * lock the PFIFO thread holds. It yields while the renderer creates shaders
 * or pipelines itself (pgraph_vk_prewarm_note_activity), runs at
 * USER_INITIATED, never under icount, and is stopped and joined at renderer
 * teardown, at shutdown and at exit. A process that dies while the worker
 * runs leaves a marker, and the next launch sets that manifest aside.
 *
 * The schema is a build-time hash of the key headers (vk/keys.h, the GLSL
 * state headers and their register enums: scripts/nv2a-key-schema.py), so
 * a build that changes what a key's bytes mean starts a new manifest.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/crc32c.h"
#include "qemu/fast-hash.h"
#include "exec/icount.h"
#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"
#include "hw/xbox/nv2a/pgraph/vk/nv2a-key-schema.h"
#include "ui/xemu-settings.h"
#include <glib/gstdio.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define PREWARM_MAGIC "XVKPRW01"
#define PREWARM_CAP 2048            /* records the worker replays */
#define PREWARM_FLUSH_BYTES (256 * 1024)
#define PREWARM_QUIET_NS 1000000000ll    /* append 1 s after the last miss */
#define PREWARM_MAX_AGE_NS 30000000000ll /* or once a record is 30 s old */
#define PREWARM_YIELD_NS 10000000ll      /* worker waits out 10 ms of misses */
#define PREWARM_MAX_FAILURES 8
#define PREWARM_MAX_WORKERS 4

typedef struct PrewarmHeader {
    char magic[8];
    uint64_t schema;
    uint32_t key_size;
    uint32_t driver_id;
} PrewarmHeader;

typedef struct PrewarmRecord {
    uint32_t crc;       /* crc32c of key */
    uint32_t reserved;
    PipelineKey key;
} PrewarmRecord;

typedef struct VkPrewarm {
    PGRAPHVkState *r;
    char *path;              /* the manifest */
    char *marker;            /* <path>.<pid>.running while the worker runs */
    GHashTable *known;       /* fast_hash of every key in the file or pending */
    GByteArray *pending;     /* records not appended yet */
    int64_t oldest_pending_ns;

    /* The workers': set before they start, read-only after */
    PipelineKey *keys;
    unsigned int num_keys;
    QemuThread threads[PREWARM_MAX_WORKERS];
    int num_workers;
    bool thread_live;        /* PFIFO/teardown side: started, not joined */
    int64_t started_ns;

    /* Shared, atomic */
    int next_worker;         /* each worker takes keys worker, + num_workers */
    int workers_live;        /* the last one out reports */
    bool stop;
    bool done;
    int64_t activity_ns;     /* last renderer shader/pipeline creation */
    uint32_t cache_misses;   /* worker creations that added to the cache */
    uint32_t built, failed;
} VkPrewarm;

static VkPrewarm *prewarm_live;   /* for the exit backstop */

static const char *driver_name(PGRAPHVkState *r)
{
    switch (r->driver_id) {
    case VK_DRIVER_ID_MOLTENVK:
        return "moltenvk";
#ifdef __APPLE__   /* older Vulkan headers elsewhere lack the id */
    case VK_DRIVER_ID_MESA_KOSMICKRISP:
        return "kosmickrisp";
#endif
    default:
        return NULL;
    }
}

static bool pid_alive(int pid)
{
#ifdef _WIN32
    return true;   /* no crash guard on Windows: markers are left alone */
#else
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
#endif
}

/*
 * A worker that was running when its process died (a crash in a driver's
 * compile of a recorded key, most likely): set its manifest aside, so the
 * same key can't take every launch down.
 */
static void check_dead_workers(VkPrewarm *p)
{
    g_autofree char *dir = g_path_get_dirname(p->path);
    g_autofree char *base = g_path_get_basename(p->path);
    g_autofree char *prefix = g_strdup_printf("%s.", base);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) {
        return;
    }
    const char *name;
    bool crashed = false;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_prefix(name, prefix) ||
            !g_str_has_suffix(name, ".running")) {
            continue;
        }
        int pid = atoi(name + strlen(prefix));
        if (pid_alive(pid)) {
            continue;
        }
        g_autofree char *m = g_build_filename(dir, name, NULL);
        g_unlink(m);
        crashed = true;
    }
    g_dir_close(d);
    if (crashed) {
        g_autofree char *bad = g_strdup_printf("%s.bad", p->path);
        g_rename(p->path, bad);
        fprintf(stderr, "xemu: vk prewarm: a launch died while rebuilding "
                        "pipelines; %s set aside\n", bad);
    }
}

static void header_init(PGRAPHVkState *r, PrewarmHeader *h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, PREWARM_MAGIC, sizeof(h->magic));
    h->schema = NV2A_KEY_SCHEMA;
    h->key_size = sizeof(PipelineKey);
    h->driver_id = r->driver_id;
}

static uint64_t key_hash(const PipelineKey *key)
{
    return fast_hash((const uint8_t *)key, sizeof(*key));
}

/* Rewrite the manifest as @recs (oldest first): compaction */
static void rewrite_manifest(VkPrewarm *p, GArray *recs)
{
    PrewarmHeader h;
    header_init(p->r, &h);
    g_autofree char *tmp = g_strdup_printf("%s.%u.tmp", p->path,
                                           (unsigned)getpid());
    FILE *f = qemu_fopen(tmp, "wb");
    if (!f) {
        return;
    }
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    if (recs->len) {
        ok &= fwrite(recs->data, sizeof(PrewarmRecord), recs->len, f) ==
              recs->len;
    }
    ok &= fclose(f) == 0;
    if (!ok || g_rename(tmp, p->path) != 0) {
        g_unlink(tmp);
    }
}

/*
 * Read the manifest: every intact record's key joins the known set, and the
 * newest PREWARM_CAP distinct keys for this surface scale become the
 * worker's list, newest first. A manifest of another schema, key size or
 * driver is replaced; a torn tail or a bad record is skipped.
 */
static void load_manifest(VkPrewarm *p, PGRAPHState *pg)
{
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(p->path, &data, &len, NULL)) {
        return;
    }
    PrewarmHeader want, *h = (PrewarmHeader *)data;
    header_init(p->r, &want);
    if (len < sizeof(*h) || memcmp(h, &want, sizeof(want))) {
        g_free(data);
        g_unlink(p->path);
        return;
    }
    size_t n = (len - sizeof(*h)) / sizeof(PrewarmRecord);
    const PrewarmRecord *recs = (const PrewarmRecord *)(data + sizeof(*h));
    GArray *keep = g_array_new(FALSE, FALSE, sizeof(PrewarmRecord));
    p->keys = g_new(PipelineKey, MIN(n, PREWARM_CAP));
    size_t bad = 0;
    for (size_t i = n; i-- > 0;) {
        const PrewarmRecord *rec = &recs[i];
        if (crc32c(0xffffffff, (const uint8_t *)&rec->key, sizeof(rec->key)) !=
            rec->crc) {
            bad++;
            continue;
        }
        uint64_t hash = key_hash(&rec->key);
        if (g_hash_table_contains(p->known, &hash)) {
            continue;
        }
        uint64_t *k = g_new(uint64_t, 1);
        *k = hash;
        g_hash_table_add(p->known, k);
        if (keep->len < PREWARM_CAP) {
            g_array_prepend_val(keep, *rec);
        }
        if (p->num_keys < PREWARM_CAP && !rec->key.clear &&
            rec->key.shader_state.vsh.surface_scale_factor ==
                pg->surface_scale_factor) {
            p->keys[p->num_keys++] = rec->key;
        }
    }
    if (bad || n > 2 * PREWARM_CAP || n > keep->len + keep->len / 2) {
        rewrite_manifest(p, keep);
    }
    g_array_free(keep, TRUE);
    g_free(data);
}

static void flush_pending(VkPrewarm *p)
{
    if (!p->pending->len) {
        return;
    }
    int fd = qemu_open_old(p->path, O_WRONLY | O_APPEND | O_CREAT | O_BINARY,
                           0644);
    if (fd >= 0) {
        struct stat st;
        bool ok = fstat(fd, &st) == 0;
        if (ok && st.st_size == 0) {
            PrewarmHeader h;
            header_init(p->r, &h);
            ok = write(fd, &h, sizeof(h)) == sizeof(h);
        }
        if (ok) {
            /* A short write leaves a torn tail the loader skips */
            ssize_t w = write(fd, p->pending->data, p->pending->len);
            (void)w;
        }
        close(fd);
    }
    g_byte_array_set_size(p->pending, 0);
}

/* ---- the worker ---------------------------------------------------------- */

typedef struct WorkerModule {
    ShaderModuleCacheKey key;
    ShaderModuleInfo *info;
} WorkerModule;

static ShaderModuleInfo *worker_module(PGRAPHVkState *r, GHashTable *mods,
                                       const ShaderModuleCacheKey *key)
{
    uint64_t h = fast_hash((const uint8_t *)key, sizeof(*key));
    GPtrArray *bucket = g_hash_table_lookup(mods, &h);
    if (bucket) {
        for (guint i = 0; i < bucket->len; i++) {
            WorkerModule *m = g_ptr_array_index(bucket, i);
            if (!memcmp(&m->key, key, sizeof(*key))) {
                return m->info;
            }
        }
    } else {
        uint64_t *k = g_new(uint64_t, 1);
        *k = h;
        bucket = g_ptr_array_new();
        g_hash_table_insert(mods, k, bucket);
    }
    MString *code = pgraph_vk_gen_glsl_for_module_key(key);
    WorkerModule *m = g_new(WorkerModule, 1);
    m->key = *key;
    m->info = pgraph_vk_create_shader_module_from_glsl(r, key->kind,
                                                       mstring_get_str(code));
    mstring_unref(code);
    g_ptr_array_add(bucket, m);
    return m->info;
}

typedef struct WorkerPass {
    RenderPassState state;
    VkRenderPass pass;
} WorkerPass;

static VkRenderPass worker_pass(PGRAPHVkState *r, GArray *passes,
                                const RenderPassState *state)
{
    for (guint i = 0; i < passes->len; i++) {
        WorkerPass *wp = &g_array_index(passes, WorkerPass, i);
        if (!memcmp(&wp->state, state, sizeof(*state))) {
            return wp->pass;
        }
    }
    WorkerPass wp = { .state = *state,
                      .pass = pgraph_vk_create_render_pass(r, state) };
    g_array_append_val(passes, wp);
    return wp.pass;
}

static void worker_yield(VkPrewarm *p)
{
    for (;;) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (qatomic_read(&p->stop) ||
            now - qatomic_read(&p->activity_ns) >= PREWARM_YIELD_NS) {
            return;
        }
        g_usleep(2000);
    }
}

static void *prewarm_thread(void *opaque)
{
    VkPrewarm *p = opaque;
    PGRAPHVkState *r = p->r;
    int worker = qatomic_fetch_inc(&p->next_worker);

#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    nsprof_exclude_thread();

    GHashTable *mods = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, NULL);
    GArray *passes = g_array_new(FALSE, FALSE, sizeof(WorkerPass));

    for (unsigned int i = worker; i < p->num_keys; i += p->num_workers) {
        worker_yield(p);
        if (qatomic_read(&p->stop)) {
            break;
        }
        const PipelineKey *key = &p->keys[i];
        ShaderModuleCacheKey mk[3];
        pgraph_vk_shader_module_keys(r, &key->shader_state, mk);
        VkShaderModule vm[3] = {
            worker_module(r, mods, &mk[0])->module,
            mk[1].kind ? worker_module(r, mods, &mk[1])->module :
                         VK_NULL_HANDLE,
            worker_module(r, mods, &mk[2])->module,
        };
        VkRenderPass pass = worker_pass(r, passes, &key->render_pass_state);

        PipelineCacheFeedback fb;
        const void *pnext;
        uint32_t stages;
        VkPipeline pipeline;
        VkPipelineLayout layout;
        if (pgraph_vk_build_draw_pipeline(r, key, vm, pass, &fb, &pnext,
                                          &stages, &pipeline,
                                          &layout) == VK_SUCCESS) {
            if (pgraph_vk_pipeline_cache_feedback_is_miss(r, pnext, stages,
                                                          &fb)) {
                qatomic_inc(&p->cache_misses);
            }
            vkDestroyPipeline(r->device, pipeline, NULL);
            vkDestroyPipelineLayout(r->device, layout, NULL);
            qatomic_inc(&p->built);
        } else if (qatomic_fetch_inc(&p->failed) + 1 >= PREWARM_MAX_FAILURES) {
            break;
        }
    }

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, mods);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GPtrArray *bucket = v;
        for (guint i = 0; i < bucket->len; i++) {
            WorkerModule *m = g_ptr_array_index(bucket, i);
            pgraph_vk_destroy_shader_module(r, m->info);
            g_free(m);
        }
        g_ptr_array_free(bucket, TRUE);
    }
    g_hash_table_destroy(mods);
    for (guint i = 0; i < passes->len; i++) {
        vkDestroyRenderPass(r->device,
                            g_array_index(passes, WorkerPass, i).pass, NULL);
    }
    g_array_free(passes, TRUE);

    if (qatomic_fetch_dec(&p->workers_live) != 1) {
        return NULL;
    }
    if (p->marker) {
        g_unlink(p->marker);
    }
    fprintf(stderr, "xemu: vk prewarm: %u of %u recorded pipelines rebuilt in "
                    "%.1f s (%u new to the pipeline cache, %u failed)\n",
            qatomic_read(&p->built), p->num_keys,
            (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - p->started_ns) / 1e9,
            qatomic_read(&p->cache_misses), qatomic_read(&p->failed));
    qatomic_store_release(&p->done, true);
    return NULL;
}

/* ---- the renderer's side ------------------------------------------------- */

static void prewarm_join(VkPrewarm *p)
{
    if (p && p->thread_live) {
        qatomic_set(&p->stop, true);
        for (int i = 0; i < p->num_workers; i++) {
            qemu_thread_join(&p->threads[i]);
        }
        p->thread_live = false;
    }
}

static void prewarm_exit_backstop(void)
{
    /* Before MoltenVK's and glslang's static destructors run */
    prewarm_join(qatomic_read(&prewarm_live));
}

void pgraph_vk_prewarm_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    const char *driver = driver_name(r);

    r->prewarm = NULL;
    if (!g_config.perf.cache_shaders || !driver) {
        return;
    }

    VkPrewarm *p = g_new0(VkPrewarm, 1);
    p->r = r;
    p->path = g_strdup_printf("%svk_prewarm-%s-%016" PRIx64 ".bin",
                              xemu_settings_get_base_path(), driver,
                              (uint64_t)NV2A_KEY_SCHEMA);
    p->known = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                     NULL);
    p->pending = g_byte_array_new();
    r->prewarm = p;

    check_dead_workers(p);
    load_manifest(p, pg);

    /* Deterministic runs neither need it nor want a thread beside them */
    if (!p->num_keys || icount_enabled()) {
        return;
    }
#ifndef _WIN32
    p->marker = g_strdup_printf("%s.%d.running", p->path, (int)getpid());
    g_file_set_contents(p->marker, "", 0, NULL);
#endif
    /*
     * Metal's compiler scales only so far: 226 pipelines rebuilt with the
     * Metal cache cold took 10.4 s on 1 worker, 7.6 on 2, 6.6 on 4 and 6.2
     * on 8 (M2 Ultra, 2026-10-24). Up to 4 workers, 2 on an 8-core Mac.
     */
    p->num_workers = MIN(PREWARM_MAX_WORKERS,
                         MAX(1, (int)g_get_num_processors() / 4));
    p->workers_live = p->num_workers;
    p->started_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for (int i = 0; i < p->num_workers; i++) {
        qemu_thread_create(&p->threads[i], "vk.prewarm", prewarm_thread, p,
                           QEMU_THREAD_JOINABLE);
    }
    p->thread_live = true;
    qatomic_set(&prewarm_live, p);
    static bool registered;
    if (!registered) {
        registered = true;
        atexit(prewarm_exit_backstop);
    }
}

/* The renderer is creating a shader or pipeline: the worker yields. */
void pgraph_vk_prewarm_note_activity(PGRAPHVkState *r)
{
    if (r->prewarm) {
        qatomic_set(&r->prewarm->activity_ns,
                    qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    }
}

/* A draw pipeline built on a real miss: record its key once. */
void pgraph_vk_prewarm_note_miss(PGRAPHVkState *r, const PipelineKey *key)
{
    VkPrewarm *p = r->prewarm;
    if (!p) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    qatomic_set(&p->activity_ns, now);
    uint64_t hash = key_hash(key);
    if (g_hash_table_contains(p->known, &hash)) {
        return;
    }
    uint64_t *k = g_new(uint64_t, 1);
    *k = hash;
    g_hash_table_add(p->known, k);

    PrewarmRecord rec = { .key = *key };
    rec.crc = crc32c(0xffffffff, (const uint8_t *)&rec.key, sizeof(rec.key));
    if (!p->pending->len) {
        p->oldest_pending_ns = now;
    }
    g_byte_array_append(p->pending, (const guint8 *)&rec, sizeof(rec));
}

/* Each flip: append what settled, credit the worker's cache additions. */
void pgraph_vk_prewarm_flip(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkPrewarm *p = r->prewarm;
    if (!p) {
        return;
    }
    uint32_t misses = qatomic_xchg(&p->cache_misses, 0);
    if (misses) {
        if (!r->pipeline_cache_unsaved) {
            r->pipeline_cache_first_miss_ns =
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        r->pipeline_cache_unsaved += misses;
    }
    if (p->thread_live && qatomic_load_acquire(&p->done)) {
        prewarm_join(p);
    }
    if (!p->pending->len) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (p->pending->len >= PREWARM_FLUSH_BYTES ||
        now - qatomic_read(&p->activity_ns) >= PREWARM_QUIET_NS ||
        now - p->oldest_pending_ns >= PREWARM_MAX_AGE_NS) {
        flush_pending(p);
    }
}

/* Shutdown, first half: the worker stops at its next key. */
void pgraph_vk_prewarm_stop(PGRAPHVkState *r)
{
    if (r && r->prewarm) {
        qatomic_set(&r->prewarm->stop, true);
    }
}

/* Teardown (before the device, caches and glslang go): join, flush, free. */
void pgraph_vk_prewarm_finalize(PGRAPHVkState *r)
{
    VkPrewarm *p = r->prewarm;
    if (!p) {
        return;
    }
    prewarm_join(p);
    qatomic_set(&prewarm_live, NULL);
    flush_pending(p);
    g_byte_array_free(p->pending, TRUE);
    g_hash_table_destroy(p->known);
    g_free(p->keys);
    g_free(p->marker);
    g_free(p->path);
    g_free(p);
    r->prewarm = NULL;
}

/* Shutdown, second half (BQL dropped): join and keep what was recorded. */
void pgraph_vk_prewarm_shutdown_wait(PGRAPHVkState *r)
{
    VkPrewarm *p = r ? r->prewarm : NULL;
    if (!p) {
        return;
    }
    prewarm_join(p);
}
