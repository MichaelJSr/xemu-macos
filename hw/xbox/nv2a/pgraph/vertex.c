/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "nv2a_vsh_emulator.h"
#include "nv2a_vsh_disassembler.h"
#include "gpu_counters.h"

void pgraph_update_inline_value(PGRAPHState *pg, VertexAttribute *attr,
                                const uint8_t *data)
{
    assert(attr->count <= 4);

    /*
     * Compute the new value into a local first. inline_value feeds
     * set_vsh_uniform_values via pgraph_get_inline_values, so a write
     * here needs pgraph_mark_uniforms_dirty — but the existing call
     * sites run on every draw even when the guest hasn't changed the
     * source data. memcmp-before-write narrows the dirty mark to
     * actual value changes, preserving the gate's effectiveness on
     * titles that stream the same vertex data across back-to-back
     * draws.
     */
    float new_value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            for (uint32_t i = 0; i < attr->count; ++i) {
                new_value[i] = (float)data[i] / 255.0f;
            }
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1: {
            const int16_t *val = (const int16_t *) data;
            for (uint32_t i = 0; i < attr->count; ++i, ++val) {
                new_value[i] = MAX(-1.0f, (float) *val / 32767.0f);
            }
            break;
        }
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            memcpy(new_value, data, attr->size * attr->count);
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K: {
            const int16_t *val = (const int16_t *) data;
            for (uint32_t i = 0; i < attr->count; ++i, ++val) {
                new_value[i] = (float)*val;
            }
            break;
        }
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            const int32_t val = *(const int32_t *)data;
            int32_t x = val & 0x7FF;
            if (x & 0x400) {
                x |= 0xFFFFF800;
            }
            int32_t y = (val >> 11) & 0x7FF;
            if (y & 0x400) {
                y |= 0xFFFFF800;
            }
            int32_t z = (val >> 22) & 0x7FF;
            if (z & 0x200) {
                z |= 0xFFFFFC00;
            }

            new_value[0] = MAX(-1.0f, (float)x / 1023.0f);
            new_value[1] = MAX(-1.0f, (float)y / 1023.0f);
            new_value[2] = MAX(-1.0f, (float)z / 511.0f);
            break;
        }
    default:
        fprintf(stderr, "Unknown vertex attribute type: for format 0x%x\n",
                attr->format);
        assert(!"Unsupported attribute type");
        break;
    }

    if (memcmp(attr->inline_value, new_value, sizeof(new_value)) != 0) {
        memcpy(attr->inline_value, new_value, sizeof(new_value));
        pgraph_mark_uniforms_dirty(pg);
    }
}

void pgraph_get_inline_values(PGRAPHState *pg, uint16_t attrs,
                               float values[NV2A_VERTEXSHADER_ATTRIBUTES][4],
                               int *count)
{
    int num_attributes = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (attrs & (1 << i)) {
            memcpy(values[num_attributes],
                   pg->vertex_attributes[i].inline_value, 4 * sizeof(float));
            num_attributes += 1;
        }
    }

    if (count) {
        *count = num_attributes;
    }
}


void pgraph_allocate_inline_buffer_vertices(PGRAPHState *pg, unsigned int attr)
{
    /*
     * Every NV097_SET_VERTEX_DATA* / SET_VERTEX*F / SET_NORMAL3F / ...
     * method handler that writes attr->inline_value calls through
     * this function first. Marking here (before the early-return)
     * guarantees every method that subsequently touches inline_value
     * will also mark shader uniforms dirty — the inline_value is a
     * potential input to pgraph_glsl_set_vsh_uniform_values'
     * pgraph_get_inline_values, so the UBO must re-pull on change.
     */
    pgraph_mark_uniforms_dirty(pg);

    VertexAttribute *attribute = &pg->vertex_attributes[attr];

    if (attribute->inline_buffer_populated || pg->inline_buffer_length == 0) {
        return;
    }

    /* Now upload the previous attribute value */
    attribute->inline_buffer_populated = true;
    for (int i = 0; i < pg->inline_buffer_length; i++) {
        memcpy(&attribute->inline_buffer[i * 4], attribute->inline_value,
               sizeof(float) * 4);
    }
}

void pgraph_finish_inline_buffer_vertex(PGRAPHState *pg)
{
    pgraph_check_within_begin_end_block(pg);
    if (unlikely(pg->gc_on)) {
        nv2a_gc_immediate_vertex(pg);
    }
    assert(pg->inline_buffer_length < NV2A_MAX_BATCH_LENGTH);

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attribute = &pg->vertex_attributes[i];
        if (attribute->inline_buffer_populated) {
            memcpy(&attribute->inline_buffer[pg->inline_buffer_length * 4],
                   attribute->inline_value, sizeof(float) * 4);
        }
    }

    pg->inline_buffer_length++;
}

/* After a load: the saved indices' range, which the save doesn't hold. */
void pgraph_rescan_inline_elements(PGRAPHState *pg)
{
    uint32_t lo = UINT32_MAX, hi = 0;

    for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
        lo = MIN(lo, pg->inline_elements[i]);
        hi = MAX(hi, pg->inline_elements[i]);
    }
    pg->inline_elements_min = lo;
    pg->inline_elements_max = hi;
}

void pgraph_reset_inline_buffers(PGRAPHState *pg)
{
    pg->inline_elements_length = 0;
    pg->inline_array_length = 0;
    pg->inline_buffer_length = 0;
    pgraph_reset_draw_arrays(pg);
}

void pgraph_reset_draw_arrays(PGRAPHState *pg)
{
    pg->draw_arrays_length = 0;
    pg->draw_arrays_min_start = -1;
    pg->draw_arrays_max_count = 0;
    pg->draw_arrays_prevent_connect = false;
}

/*
 * Fork: the carryover's slices, by program content: the steps that can reach
 * the final oFog (vsh_carry_build_slice), last marked final, or none, for
 * the words they were parsed from, through the final step (the parse reads
 * nothing else). Azurik loads one of 20-54 programs into the same slots
 * dozens of times a frame, which upstream's per-slot cache answered with a
 * parse each time; here a program comes back with its slice already built.
 * One table for the process (one NV2A), dropped whole when full.
 */
typedef struct VshCarryKey {
    uint32_t hash;
    uint32_t nslots;
    const uint32_t (*words)[VSH_TOKEN_SIZE];
} VshCarryKey;

typedef struct VshCarrySlice {
    VshCarryKey key;
    Nv2aVshProgram prog;
    bool upstream_path; /* run the whole program as upstream does */
    uint32_t words[][VSH_TOKEN_SIZE];
} VshCarrySlice;

/*
 * What vsh_carry_cached_programs points at, per program start. Its steps
 * stay NULL (pgraph_vsh_carryover_invalidate_cache frees only the entry);
 * a program upload marks it stale, and the next draw from that start checks
 * the words.
 */
typedef struct VshCarryProgram {
    Nv2aVshProgram unused;
    bool stale;
    VshCarrySlice *slice;
} VshCarryProgram;

#define VSH_CARRY_MAX_SLICES 1024
static GHashTable *vsh_carry_slices;

static guint vsh_carry_key_hash(gconstpointer k)
{
    return ((const VshCarryKey *)k)->hash;
}

static gboolean vsh_carry_key_equal(gconstpointer a, gconstpointer b)
{
    const VshCarryKey *ka = a, *kb = b;
    return ka->nslots == kb->nslots &&
           !memcmp(ka->words, kb->words, ka->nslots * sizeof(ka->words[0]));
}

static void vsh_carry_slice_free(gpointer p)
{
    VshCarrySlice *cs = p;
    nv2a_vsh_program_destroy(&cs->prog);
    g_free(cs);
}

enum {
    VSH_CARRY_R12 = 12, /* oPos, which temporary register 12 reads */
    VSH_CARRY_FOG = 13,
    VSH_CARRY_A0 = 14,
};

/* The live-set bit a step output writes, or -1 if nothing reads it back. */
static int vsh_carry_out_reg(const Nv2aVshOutput *out, bool *odd)
{
    switch (out->type) {
    case NV2ART_TEMPORARY:
        if (out->index < 12) {
            return out->index;
        }
        break;
    case NV2ART_OUTPUT:
        if (out->index == NV2AOR_POS) {
            return VSH_CARRY_R12;
        } else if (out->index == NV2AOR_FOG_COORD) {
            return VSH_CARRY_FOG;
        } else if (out->index < 13) {
            return -1;
        }
        break;
    case NV2ART_ADDRESS:
        return VSH_CARRY_A0;
    default:
        break;
    }
    *odd = true; /* constant writes, out-of-range registers */
    return -1;
}

static uint32_t vsh_carry_in_regs(const Nv2aVshInput *in, bool *odd)
{
    switch (in->type) {
    case NV2ART_TEMPORARY:
        if (in->index <= 12) {
            return 1u << in->index;
        }
        break;
    case NV2ART_INPUT:
        return 0;
    case NV2ART_CONTEXT:
        return in->is_relative ? 1u << VSH_CARRY_A0 : 0;
    default:
        break;
    }
    *odd = true;
    return 0;
}

/*
 * Backward liveness from the final oFog over registers (r0-r11, oPos, oFog,
 * a0): a step is kept when it writes a register live after it; a kept step
 * kills what it writes whole (xyzw) and makes live what either of its halves
 * reads (both read before either writes). A dropped step writes nothing a
 * kept step reads, so the kept steps compute oFog bit for bit as the whole
 * program does. Anything unusual (constant writes, out-of-range registers)
 * takes the upstream path.
 */
static void vsh_carry_build_slice(VshCarrySlice *cs, const Nv2aVshStep *steps,
                                  uint32_t nsteps)
{
    bool keep[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
    uint32_t live = 1u << VSH_CARRY_FOG;
    uint32_t nkeep = 0;
    bool odd = false;

    for (int i = nsteps - 1; i >= 0; i--) {
        const Nv2aVshOperation *ops[2] = { &steps[i].mac, &steps[i].ilu };
        uint32_t writes = 0, kills = 0, uses = 0;
        for (int o = 0; o < 2; o++) {
            if (!ops[o]->opcode) {
                continue;
            }
            for (int k = 0; k < 2; k++) {
                const Nv2aVshOutput *out = &ops[o]->outputs[k];
                if (out->type == NV2ART_NONE) {
                    continue;
                }
                int r = vsh_carry_out_reg(out, &odd);
                if (r >= 0) {
                    writes |= 1u << r;
                    if (out->writemask == NV2AWM_XYZW && r != VSH_CARRY_A0) {
                        kills |= 1u << r;
                    }
                }
            }
            if (o == 0) {
                for (int k = 0; k < 3; k++) {
                    if (ops[o]->inputs[k].type == NV2ART_NONE) {
                        break;
                    }
                    uses |= vsh_carry_in_regs(&ops[o]->inputs[k], &odd);
                }
            } else {
                uses |= vsh_carry_in_regs(&ops[o]->inputs[0], &odd);
            }
        }
        keep[i] = writes & live;
        if (keep[i]) {
            live = (live & ~kills) | uses;
            nkeep++;
        }
    }

    cs->upstream_path = odd;
    if (odd) {
        nkeep = nsteps;
    }
    if (!nkeep) {
        return; /* nothing reaches oFog: the carried value stays */
    }
    cs->prog.steps = malloc(sizeof(Nv2aVshStep) * nkeep);
    Nv2aVshStep *dst = cs->prog.steps;
    for (uint32_t i = 0; i < nsteps; i++) {
        if (odd || keep[i]) {
            *dst = steps[i];
            dst->is_final = false;
            dst++;
        }
    }
    dst[-1].is_final = true;
}

/* The slice for the program at program_start; NULL where the parse fails. */
static VshCarrySlice *vsh_carry_slice_get(PGRAPHState *pg, uint32_t program_start)
{
    const uint32_t (*words)[VSH_TOKEN_SIZE] = &pg->program_data[program_start];
    uint32_t avail = NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - program_start;
    uint32_t nslots = 0;
    while (nslots < avail && !(words[nslots++][3] & 1)) {
        /* the final flag, as nv2a_vsh_parse_step reads it */
    }
    uint32_t hash = nslots;
    const uint32_t *end = words[0] + nslots * VSH_TOKEN_SIZE;
    for (const uint32_t *w = words[0]; w < end; w++) {
        hash = (hash ^ *w) * 0x01000193;
    }
    VshCarryKey key = { hash, nslots, words };

    if (!vsh_carry_slices) {
        vsh_carry_slices = g_hash_table_new_full(
            vsh_carry_key_hash, vsh_carry_key_equal, NULL, vsh_carry_slice_free);
    }
    VshCarrySlice *cs = g_hash_table_lookup(vsh_carry_slices, &key);
    if (cs) {
        return cs;
    }

    Nv2aVshProgram program;
    if (nv2a_vsh_parse_program(&program, words[0], avail) != NV2AVPR_SUCCESS) {
        return NULL;
    }
    if (g_hash_table_size(vsh_carry_slices) >= VSH_CARRY_MAX_SLICES) {
        pgraph_vsh_carryover_invalidate_cache(pg);
        g_hash_table_remove_all(vsh_carry_slices);
    }
    cs = g_malloc0(sizeof(*cs) + nslots * sizeof(cs->words[0]));
    memcpy(cs->words, words, nslots * sizeof(cs->words[0]));
    cs->key = (VshCarryKey){ hash, nslots, cs->words };
    vsh_carry_build_slice(cs, program.steps, nslots);
    nv2a_vsh_program_destroy(&program);
    g_hash_table_add(vsh_carry_slices, cs);
    return cs;
}

void pgraph_vsh_carryover_invalidate_cache(PGRAPHState *pg)
{
    for (int i = 0; i < ARRAY_SIZE(pg->vsh_carry_cached_programs); ++i) {
        if (pg->vsh_carry_cached_programs[i]) {
            nv2a_vsh_program_destroy(pg->vsh_carry_cached_programs[i]);
            g_free(pg->vsh_carry_cached_programs[i]);
            pg->vsh_carry_cached_programs[i] = NULL;
        }
    }
}

void pgraph_vsh_carryover_reset(PGRAPHState *pg)
{
    pg->vsh_carry_fog[0] = 0.0f;
    pg->vsh_carry_fog[1] = 0.0f;
    pg->vsh_carry_fog[2] = 0.0f;
    pg->vsh_carry_fog[3] = 1.0f;
    pgraph_vsh_carryover_invalidate_cache(pg);
    pg->vsh_carry_cache_dirty = false;
}

void pgraph_vsh_carryover_update(PGRAPHState *pg)
{
    if (!pg->draw_arrays_length && !pg->inline_elements_length &&
        !pg->inline_buffer_length && !pg->inline_array_length) {
        return;
    }

    if (pg->vsh_carry_cache_dirty) {
        for (int i = 0; i < ARRAY_SIZE(pg->vsh_carry_cached_programs); ++i) {
            if (pg->vsh_carry_cached_programs[i]) {
                ((VshCarryProgram *)pg->vsh_carry_cached_programs[i])->stale =
                    true;
            }
        }
        pg->vsh_carry_cache_dirty = false;
    }

    bool is_vertex_program = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                      NV_PGRAPH_CSV0_D_MODE) == 2;
    if (!is_vertex_program) {
        return;
    }

    uint32_t program_start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                                      NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (program_start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return;
    }

    Nv2aVshProgram **slot = &pg->vsh_carry_cached_programs[program_start];
    VshCarryProgram *cp = (VshCarryProgram *)*slot;
    if (cp && cp->stale) {
        VshCarrySlice *cs = cp->slice;
        if (!memcmp(cs->words, pg->program_data[program_start],
                    cs->key.nslots * sizeof(cs->words[0]))) {
            cp->stale = false;
        } else {
            g_free(cp);
            *slot = NULL;
            cp = NULL;
        }
    }
    if (!cp) {
        VshCarrySlice *cs = vsh_carry_slice_get(pg, program_start);
        if (!cs) {
            return;
        }
        cp = g_new0(VshCarryProgram, 1);
        cp->slice = cs;
        *slot = &cp->unused;
    }
    const VshCarrySlice *cs = cp->slice;
    if (!cs->prog.steps) {
        return;
    }

    /*
     * Upstream's state: zero temporaries and a0, outputs (0, 0, 0, 1) with
     * the carried oFog, the inline attribute values. The slice reads the
     * constants in place; the upstream path keeps upstream's struct, since
     * an out-of-range register reads past its array.
     */
    Nv2aVshCPUFullExecutionState full;
    float inputs[16 * 4], outputs[13 * 4], temps[12 * 4] = { 0 };
    float address[4] = { 0 };
    Nv2aVshExecutionState state;
    if (cs->upstream_path) {
        state = nv2a_vsh_emu_initialize_full_execution_state(&full);
        QEMU_BUILD_BUG_MSG(sizeof(full.context_regs) !=
                               sizeof(pg->vsh_constants),
                           "context_regs and vsh_constants size mismatch");
        memcpy(full.context_regs, pg->vsh_constants,
               sizeof(full.context_regs));
    } else {
        state = (Nv2aVshExecutionState){
            .input_regs = inputs,
            .output_regs = outputs,
            .temp_regs = temps,
            .context_regs = (float *)pg->vsh_constants,
            .address_reg = address,
        };
    }

    for (int i = 0; i < ARRAY_SIZE(pg->vertex_attributes); ++i) {
        memcpy(&state.input_regs[i * 4],
               pg->vertex_attributes[i].inline_value, sizeof(float) * 4);
    }

    for (int i = 0; i < 13; ++i) {
        state.output_regs[i * 4 + 0] = 0.0f;
        state.output_regs[i * 4 + 1] = 0.0f;
        state.output_regs[i * 4 + 2] = 0.0f;
        state.output_regs[i * 4 + 3] = 1.0f;
    }
    memcpy(&state.output_regs[NV2AOR_FOG_COORD * 4], pg->vsh_carry_fog,
           sizeof(pg->vsh_carry_fog));

    nv2a_vsh_emu_execute(&state, &cs->prog);

    /*
     * No pgraph_mark_uniforms_dirty: only a program that writes oFog changes
     * it, and such a program sets oFog.x whatever the mask (fog_mask_str),
     * so its shader never reads vshCarryFog; the uniform skip-gate keeps the
     * binding only while that program stays bound, and any other binding
     * pulls the new value (shader_bindings_changed).
     */
    memcpy(pg->vsh_carry_fog, &state.output_regs[NV2AOR_FOG_COORD * 4],
           sizeof(pg->vsh_carry_fog));
}
