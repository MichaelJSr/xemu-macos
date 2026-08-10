/*
 * Helper FP conversions
 *
 * Copyright (c) 2020-2025 Matt Borgerson
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


#ifndef FLOATCONV_H
#define FLOATCONV_H

#include <stdint.h>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

static inline float int8_to_float(int8_t x)
{
    return x / 128.0f;
}

static inline float uint8_to_float(uint8_t value)
{
    return ((int)value - 0x80) / (1.0 * 0x80);
}

static inline float int16_to_float(int16_t value)
{
    return value / (1.0 * 0x8000);
}

static inline float s6p9_to_float(int16_t value)
{
    return value / 512.0f;
}

static inline float int32_to_float(int32_t value)
{
    return value / (1.0 * 0x80000000);
}

static inline float int24_to_float(int32_t value)
{
    return int32_to_float((uint32_t)value << 8);
}

static inline uint32_t float_to_24b(float value)
{
    double scaled_value = value * (8.0 * 0x100000);
    int int24;
    if (scaled_value >= (1.0 * 0x7fffff)) {
        int24 = 0x7fffff;
    } else if (scaled_value <= (-8.0 * 0x100000)) {
        int24 = -1 - 0x7fffff;
    } else {
        int24 = lrint(scaled_value);
    }
    return int24 & 0xffffff;
}

static inline void float_to_24b_bulk(const float *src, uint32_t *dst, int count)
{
#if defined(__aarch64__) && defined(__ARM_NEON)
    const float32x4_t scale = vdupq_n_f32(8.0f * 0x100000);
    const int32x4_t max_val = vdupq_n_s32(0x7fffff);
    const int32x4_t min_val = vdupq_n_s32(-0x800000);
    const int32x4_t mask = vdupq_n_s32(0xffffff);

    int i = 0;
    for (; i + 4 <= count; i += 4) {
        float32x4_t v = vmulq_f32(vld1q_f32(src + i), scale);
        int32x4_t iv = vcvtnq_s32_f32(v);
        iv = vminq_s32(iv, max_val);
        iv = vmaxq_s32(iv, min_val);
        iv = vandq_s32(iv, mask);
        vst1q_u32(dst + i, vreinterpretq_u32_s32(iv));
    }
    for (; i < count; i++) {
        dst[i] = float_to_24b(src[i]);
    }
#elif defined(__x86_64__) || defined(_M_X64)
    /*
     * SSE2 path (baseline on x86_64) — parity with the NEON path
     * above for Windows/Linux builds. The clamp is done in the float
     * domain BEFORE the convert: _mm_cvtps_epi32 yields the
     * "integer indefinite" value 0x80000000 for out-of-range inputs
     * (positive overflow would otherwise clamp to the negative
     * rail). Both bounds are < 2^24 so they are exactly
     * representable in f32, and the convert uses MXCSR
     * round-to-nearest-even, matching lrint / vcvtnq semantics.
     */
    const __m128 scale = _mm_set1_ps(8.0f * 0x100000);
    const __m128 fmax = _mm_set1_ps(8388607.0f);   /*  0x7fffff */
    const __m128 fmin = _mm_set1_ps(-8388608.0f);  /* -0x800000 */
    const __m128i mask = _mm_set1_epi32(0xffffff);

    int i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128 v = _mm_mul_ps(_mm_loadu_ps(src + i), scale);
        v = _mm_min_ps(_mm_max_ps(v, fmin), fmax);
        __m128i iv = _mm_cvtps_epi32(v);
        iv = _mm_and_si128(iv, mask);
        _mm_storeu_si128((__m128i *)(dst + i), iv);
    }
    for (; i < count; i++) {
        dst[i] = float_to_24b(src[i]);
    }
#else
    for (int i = 0; i < count; i++) {
        dst[i] = float_to_24b(src[i]);
    }
#endif
}

#endif
