/*
 * Geforce NV2A PGRAPH Vulkan Renderer: asynchronous draw-pipeline compile
 *
 * A draw whose PipelineKey misses the in-memory pipeline LRU would otherwise
 * compile on the PFIFO thread, stalling production and the flip ack for the
 * 30-90 ms (worst observed 462 ms) the driver's SPIR-V -> MSL -> Metal
 * lowering takes. Here that miss is handed to one worker thread and the draw
 * is skipped for the frames until the build lands, so the frame time stays
 * flat; the next draw of the key installs the finished pipeline and draws
 * normally. The LRU starts empty at every launch, so every key's first draws
 * in a session (and after an eviction) take this path, even when the driver
 * cache or the prewarm would have built the pipeline in about 1 ms.
 *
 * Skipping a draw is not free: a one-shot draw (a render-to-texture at a
 * level load) is lost, a repeated one leaves its surface stale until the
 * retry, and a skipped draw adds no samples to an open occlusion query. The
 * feature is therefore opt-in and bounded: Settings > "Async pipeline
 * compilation" (perf.async_pipeline_compile, off by default) turns it on,
 * a key's build is forced synchronous ASYNC_DEADLINE_NS after it was
 * submitted (a lost or starved worker costs a bounded number of frames,
 * never a hang), a failed submit falls back at once, and no worker is
 * started under icount (deterministic runs want no thread beside them).
 *
 * The worker builds from the key alone, through the renderer's own builder
 * (pgraph_vk_build_draw_pipeline) and module path, so it makes exactly what
 * the renderer would. It touches only the device, the shared VkPipelineCache
 * (internally synchronized), init-time renderer fields and objects of its
 * own; never PGRAPH state or the renderer's single-threaded caches. The
 * finished handle is installed by the PFIFO thread on drain.
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
#include "exec/icount.h"
#include "renderer.h"
#include "hw/xbox/nv2a/nsprof.h"
#include "ui/xemu-settings.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

/* Jobs and finished builds the worker and the PFIFO thread exchange */
#define ASYNC_QUEUE 16
/* A submitted key is built synchronously if the worker has not answered in
 * this long, so a lost or starved worker cannot stall a key forever */
#define ASYNC_DEADLINE_NS (200ll * 1000000ll)

typedef struct AsyncJob {
    PipelineKey key;
    uint64_t hash;
    VkRenderPass render_pass;
} AsyncJob;

typedef struct AsyncResult {
    PipelineKey key;
    uint64_t hash;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkRenderPass render_pass;
    PipelineCacheFeedback feedback;
    uint32_t stages;
    bool had_feedback; /* pnext was &feedback.info (feedback valid) */
    bool ok; /* the build succeeded */
    int64_t build_ns; /* the worker's whole build (off the PFIFO thread) */
} AsyncResult;

typedef struct VkAsyncPipeline {
    PGRAPHVkState *r;

    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QemuMutex join_lock; /* one joiner: a shutdown wait or teardown */
    bool thread_live; /* started, not joined; joiners hold join_lock */
    bool stop;

    AsyncJob jobs[ASYNC_QUEUE];
    int job_head, job_count;

    AsyncResult results[ASYNC_QUEUE];
    int result_head, result_count;
} VkAsyncPipeline;

static VkAsyncPipeline *async_live; /* for the exit backstop */

static bool async_driver_ok(PGRAPHVkState *r)
{
    switch (r->driver_id) {
    case VK_DRIVER_ID_MOLTENVK:
        return true;
#ifdef __APPLE__ /* older Vulkan headers elsewhere lack the id */
    case VK_DRIVER_ID_MESA_KOSMICKRISP:
        return true;
#endif
    default:
        return false;
    }
}

/* ---- the worker ---------------------------------------------------------- */

static void async_build(VkAsyncPipeline *a, const AsyncJob *job,
                        AsyncResult *res)
{
    PGRAPHVkState *r = a->r;

    memset(res, 0, sizeof(*res));
    res->key = job->key;
    res->hash = job->hash;
    res->render_pass = job->render_pass;

    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    /*
     * Modules of our own, from the key's module keys, exactly as the prewarm
     * does: the renderer's shader-module cache is single-threaded. They are
     * dropped as soon as the pipeline that uses them is built.
     */
    ShaderModuleCacheKey mk[3];
    pgraph_vk_shader_module_keys(r, &job->key.shader_state, mk);

    ShaderModuleInfo *infos[3] = { NULL, NULL, NULL };
    VkShaderModule mods[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (int i = 0; i < 3; i++) {
        if (i == 1 && mk[1].kind == 0) {
            continue; /* no geometry stage */
        }
        MString *code = pgraph_vk_gen_glsl_for_module_key(&mk[i]);
        infos[i] = pgraph_vk_create_shader_module_from_glsl(
            r, mk[i].kind, mstring_get_str(code));
        mstring_unref(code);
        mods[i] = infos[i]->module;
    }

    PipelineCacheFeedback fb;
    const void *pnext = NULL;
    uint32_t stages = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pgraph_vk_build_draw_pipeline(r, &job->key, mods, job->render_pass, &fb,
                                      &pnext, &stages, &pipeline,
                                      &layout) == VK_SUCCESS) {
        res->ok = true;
        res->pipeline = pipeline;
        res->layout = layout;
        res->stages = stages;
        res->had_feedback = (pnext == &fb.info);
        res->feedback = fb;
        /* The copied info's own pointers must not outlive the source */
        if (res->had_feedback) {
            res->feedback.info.pNext = NULL;
            res->feedback.info.pPipelineCreationFeedback =
                &res->feedback.pipeline;
            res->feedback.info.pPipelineStageCreationFeedbacks =
                res->feedback.stages;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (infos[i]) {
            pgraph_vk_destroy_shader_module(r, infos[i]);
        }
    }

    res->build_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
}

static void *async_thread(void *opaque)
{
    VkAsyncPipeline *a = opaque;

#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    nsprof_exclude_thread();

    for (;;) {
        qemu_mutex_lock(&a->lock);
        while (a->job_count == 0 && !a->stop) {
            qemu_cond_wait(&a->cond, &a->lock);
        }
        if (a->stop) { /* queued jobs own nothing: drop them */
            qemu_mutex_unlock(&a->lock);
            break;
        }
        AsyncJob job = a->jobs[a->job_head];
        a->job_head = (a->job_head + 1) % ASYNC_QUEUE;
        a->job_count--;
        qemu_mutex_unlock(&a->lock);

        AsyncResult res;
        async_build(a, &job, &res);

        qemu_mutex_lock(&a->lock);
        if (a->result_count < ASYNC_QUEUE) {
            int idx = (a->result_head + a->result_count) % ASYNC_QUEUE;
            a->results[idx] = res;
            /* Atomic: the drain's unlocked hint reads it */
            qatomic_set(&a->result_count, a->result_count + 1);
        } else {
            /* The renderer is not draining: give the entry up. The key stays
             * pending and the deadline fallback builds it synchronously. */
            if (res.ok) {
                vkDestroyPipeline(a->r->device, res.pipeline, NULL);
                vkDestroyPipelineLayout(a->r->device, res.layout, NULL);
            }
        }
        qemu_mutex_unlock(&a->lock);
    }

    return NULL;
}

/* ---- the renderer's side ------------------------------------------------- */

static void async_join(VkAsyncPipeline *a)
{
    if (!a) {
        return;
    }
    qemu_mutex_lock(&a->join_lock);
    if (a->thread_live) {
        qemu_mutex_lock(&a->lock);
        a->stop = true;
        qemu_cond_broadcast(&a->cond);
        qemu_mutex_unlock(&a->lock);
        qemu_thread_join(&a->thread);
        a->thread_live = false;
    }
    qemu_mutex_unlock(&a->join_lock);
}

static void async_exit_backstop(void)
{
    /* Before MoltenVK's and glslang's static destructors run */
    async_join(qatomic_read(&async_live));
}

void pgraph_vk_async_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->async_compile = NULL;
    if (!g_config.perf.async_pipeline_compile || icount_enabled() ||
        !async_driver_ok(r)) {
        return;
    }

    VkAsyncPipeline *a = g_new0(VkAsyncPipeline, 1);
    a->r = r;
    qemu_mutex_init(&a->lock);
    qemu_cond_init(&a->cond);
    qemu_mutex_init(&a->join_lock);
    qemu_thread_create(&a->thread, "vk.async_pipe", async_thread, a,
                       QEMU_THREAD_JOINABLE);
    a->thread_live = true;
    r->async_compile = a;

    qatomic_set(&async_live, a);
    static bool registered;
    if (!registered) {
        registered = true;
        atexit(async_exit_backstop);
    }
}

/* Hand a first-encounter miss to the worker. False: no room or the worker
 * is stopping, build now. */
static bool async_submit(VkAsyncPipeline *a, const PipelineKey *key,
                         uint64_t hash, VkRenderPass render_pass)
{
    bool ok = false;
    qemu_mutex_lock(&a->lock);
    if (a->job_count < ASYNC_QUEUE && !a->stop) {
        int idx = (a->job_head + a->job_count) % ASYNC_QUEUE;
        a->jobs[idx].key = *key;
        a->jobs[idx].hash = hash;
        a->jobs[idx].render_pass = render_pass;
        a->job_count++;
        qemu_cond_signal(&a->cond);
        ok = true;
    }
    qemu_mutex_unlock(&a->lock);
    return ok;
}

/*
 * A draw's first-encounter miss, on the PFIFO thread. Either hand the build
 * to the worker (and let the caller skip the draw) or say to build it here:
 * no worker is running, its queue is full, or this key's build is already
 * ASYNC_DEADLINE_NS old (a lost or starved worker must not stall the key).
 */
enum AsyncMissVerdict pgraph_vk_async_miss(PGRAPHVkState *r,
                                           PipelineBinding *snode,
                                           const PipelineKey *key,
                                           uint64_t hash,
                                           VkRenderPass render_pass)
{
    VkAsyncPipeline *a = r->async_compile;
    if (!a) {
        return ASYNC_BUILD_NOW;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (snode->pending) {
        if (now - snode->pending_since_ns < ASYNC_DEADLINE_NS) {
            nsprof_event(NSPROF_EV_ASYNC_SKIP);
            return ASYNC_SKIP;
        }
        snode->pending = false;
        nsprof_event(NSPROF_EV_ASYNC_FALLBACK);
        return ASYNC_BUILD_NOW;
    }

    if (async_submit(a, key, hash, render_pass)) {
        snode->pending = true;
        snode->pending_since_ns = now;
        /* Record it so the next launch prewarms it (and the worker yields) */
        pgraph_vk_prewarm_note_miss(r, key);
        nsprof_event(NSPROF_EV_ASYNC_SUBMIT);
        return ASYNC_SKIP;
    }
    return ASYNC_BUILD_NOW;
}

static void async_install(PGRAPHState *pg, AsyncResult *res)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (res->build_ns > 0) {
        nsprof_add(NSPROF_ASYNC_BUILD, res->build_ns);
    }
    if (!res->ok) {
        nsprof_event(NSPROF_EV_ASYNC_FAIL);
        return; /* the key stays pending; the deadline builds it */
    }

    LruNode *node = lru_lookup(&r->pipeline_cache, res->hash, &res->key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->key = res->key;
    snode->pending = false;

    if (snode->pipeline != VK_NULL_HANDLE) {
        /* A synchronous fallback or a second build won the race: drop */
        nsprof_event(NSPROF_EV_ASYNC_DROP);
        vkDestroyPipeline(r->device, res->pipeline, NULL);
        vkDestroyPipelineLayout(r->device, res->layout, NULL);
        return;
    }

    snode->pipeline = res->pipeline;
    snode->layout = res->layout;
    snode->render_pass = res->render_pass;
    snode->draw_time = pg->draw_time;

    pgraph_vk_pipeline_cache_note_created(
        r, res->had_feedback ? (const void *)&res->feedback.info : NULL,
        res->stages, &res->feedback);

    nsprof_event(NSPROF_EV_ASYNC_INSTALL);
}

/* PFIFO thread, at the head of create_pipeline: install every finished
 * build. Any not installed here are installed by the draw that wants them. */
void pgraph_vk_async_drain(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkAsyncPipeline *a = r->async_compile;
    /* Unlocked hint, every draw: a stale 0 defers the install a draw */
    if (!a || qatomic_read(&a->result_count) == 0) {
        return;
    }
    for (;;) {
        qemu_mutex_lock(&a->lock);
        if (a->result_count == 0) {
            qemu_mutex_unlock(&a->lock);
            return;
        }
        AsyncResult res = a->results[a->result_head];
        a->result_head = (a->result_head + 1) % ASYNC_QUEUE;
        qatomic_set(&a->result_count, a->result_count - 1);
        qemu_mutex_unlock(&a->lock);

        async_install(pg, &res);
    }
}

/* Shutdown, first half: the worker stops at its next job boundary. */
void pgraph_vk_async_stop(PGRAPHVkState *r)
{
    VkAsyncPipeline *a = r ? r->async_compile : NULL;
    if (a) {
        qemu_mutex_lock(&a->lock);
        a->stop = true;
        qemu_cond_broadcast(&a->cond);
        qemu_mutex_unlock(&a->lock);
    }
}

/* Shutdown, second half (BQL dropped): join. */
void pgraph_vk_async_shutdown_wait(PGRAPHVkState *r)
{
    async_join(r ? r->async_compile : NULL);
}

/* Teardown (before the device and the pipeline cache go): join, free. */
void pgraph_vk_async_finalize(PGRAPHVkState *r)
{
    VkAsyncPipeline *a = r->async_compile;
    if (!a) {
        return;
    }
    async_join(a);

    /* The worker is gone: destroy what the renderer never drained */
    for (int i = 0; i < a->result_count; i++) {
        AsyncResult *res = &a->results[(a->result_head + i) % ASYNC_QUEUE];
        if (res->ok) {
            vkDestroyPipeline(r->device, res->pipeline, NULL);
            vkDestroyPipelineLayout(r->device, res->layout, NULL);
        }
    }

    qatomic_set(&async_live, NULL);
    qemu_cond_destroy(&a->cond);
    qemu_mutex_destroy(&a->lock);
    qemu_mutex_destroy(&a->join_lock);
    g_free(a);
    r->async_compile = NULL;
}
