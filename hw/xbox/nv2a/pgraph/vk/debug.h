/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_DEBUG_H
#define HW_XBOX_NV2A_PGRAPH_VK_DEBUG_H

#define DEBUG_VK 0

extern int nv2a_vk_dgroup_indent;

#define NV2A_VK_XDPRINTF(x, fmt, ...)                                  \
    do {                                                               \
        if (x) {                                                       \
            fprintf(stderr, "%*s" fmt "\n", nv2a_vk_dgroup_indent, "", \
                    ##__VA_ARGS__);                                    \
        }                                                              \
    } while (0)

#define NV2A_VK_DPRINTF(fmt, ...) NV2A_VK_XDPRINTF(DEBUG_VK, fmt, ##__VA_ARGS__)

#if DEBUG_VK
#define NV2A_VK_DGROUP_BEGIN(fmt, ...)                  \
    do {                                                \
        NV2A_VK_XDPRINTF(DEBUG_VK, fmt, ##__VA_ARGS__); \
        nv2a_vk_dgroup_indent++;                        \
    } while (0)

#define NV2A_VK_DGROUP_END(...)             \
    do {                                    \
        nv2a_vk_dgroup_indent--;            \
        nv2a_vk_assert(nv2a_vk_dgroup_indent >= 0); \
    } while (0)
#else
#define NV2A_VK_DGROUP_BEGIN(fmt, ...) ((void)0)
#define NV2A_VK_DGROUP_END(...)        ((void)0)
#endif

#define VK_CHECK(x)                                           \
    do {                                                      \
        VkResult vk_result = (x);                             \
        if (vk_result != VK_SUCCESS) {                        \
            fprintf(stderr, "vk_result = %d\n", vk_result);   \
            __builtin_trap();                                  \
        }                                                     \
    } while (0)

/* Performance build: strip internal invariant asserts from VK hot paths.
 * Guest-boundary checks (VRAM/DMA bounds) must use assert() or
 * nv2a_vk_bounds_check(), both of which stay live in perf builds.
 * Profile counter stripping is in the shared hw/xbox/nv2a/debug.h. */
#define NV2A_VK_PERF_BUILD 1

#if NV2A_VK_PERF_BUILD
#define nv2a_vk_assert(x) ((void)0)
#else
#define nv2a_vk_assert(x) assert(x)
#endif

/*
 * Bounds check for indexing/sizing driven by guest-controlled values.
 *
 * This is a real check in EVERY build, including NV2A_VK_PERF_BUILD: an
 * unlikely (statically predicted not-taken) branch plus a loud abort naming
 * the failing expression. It must never compile to __builtin_unreachable():
 * that form is not a no-op but a licence for the optimizer to assume the
 * bound, which turns a guest-driven violation (e.g. a surface size from
 * NV_PGRAPH registers exceeding the staging buffer) into a silent
 * out-of-bounds write into host-visible mapped memory instead of a
 * diagnosable failure. QEMU never defines NDEBUG, so this costs exactly what
 * the upstream plain assert() at these sites costs, and none of the sites is
 * on the per-draw hot path.
 *
 * nv2a_vk_assert() remains debug-only by design: it guards internal renderer
 * invariants that no guest input can violate.
 */
#define nv2a_vk_bounds_check(x)                                 \
    do {                                                        \
        if (G_UNLIKELY(!(x))) {                                 \
            fprintf(stderr, "%s:%d: bounds check failed: %s\n", \
                    __FILE__, __LINE__, #x);                    \
            abort();                                            \
        }                                                       \
    } while (0)

void pgraph_vk_debug_frame_terminator(void);

#endif
