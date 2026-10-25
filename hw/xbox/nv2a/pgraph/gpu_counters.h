/*
 * NV2A workload counters (QMP xemu-gpu-counters-*, xemu-frame-advance
 * gpu-counters): counted in PGRAPH's method path and the PFIFO pusher, so
 * every renderer counts the same work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_GPU_COUNTERS_H
#define HW_XBOX_NV2A_PGRAPH_GPU_COUNTERS_H

#include "exec/hwaddr.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHState PGRAPHState;

/*
 * The hooks below run only while pg->gc_on, on the thread that runs PGRAPH
 * methods (the PFIFO thread, or under icount the inline pusher's), with
 * pgraph.lock held, except nv2a_gc_pushbuf (the pusher, pfifo.lock) and
 * nv2a_gc_occlusion (any thread that delivers a report). They read PGRAPH
 * state and guest RAM and write nothing but the counters.
 */
/* PGRAPH init, before the PFIFO thread starts. */
void nv2a_gc_init(void);

void nv2a_gc_pushbuf(NV2AState *d, hwaddr base, uint32_t old_get,
                     uint32_t new_get, uint32_t word, uint32_t method_count);
void nv2a_gc_begin(PGRAPHState *pg);
void nv2a_gc_end(PGRAPHState *pg);
void nv2a_gc_draw_arrays(PGRAPHState *pg, uint32_t start, uint32_t count);
void nv2a_gc_array_element(PGRAPHState *pg, uint32_t parameter, bool pair16);
/* n > 0 ARRAY_ELEMENT16 words at @words, as n nv2a_gc_array_element calls */
void nv2a_gc_array_element16_run(PGRAPHState *pg, const uint8_t *words,
                                 size_t n);
void nv2a_gc_inline_array_word(PGRAPHState *pg);
void nv2a_gc_immediate_vertex(PGRAPHState *pg);
void nv2a_gc_clear(PGRAPHState *pg, uint32_t parameter);
void nv2a_gc_report(PGRAPHState *pg, hwaddr addr);
void nv2a_gc_semaphore(PGRAPHState *pg, hwaddr addr);
void nv2a_gc_blit(PGRAPHState *pg);
void nv2a_gc_flip(PGRAPHState *pg);
void nv2a_gc_occlusion(PGRAPHState *pg, uint32_t samples);

#endif
