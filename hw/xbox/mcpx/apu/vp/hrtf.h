/*
 * HRTF Filter
 *
 * Copyright (c) 2025 Matt Borgerson
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

#ifndef HW_XBOX_MCPX_HRTF_H
#define HW_XBOX_MCPX_HRTF_H

#include <string.h>
#include <stddef.h>
#include <math.h>
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "hw/xbox/mcpx/apu/apu_regs.h"

#define HRTF_SAMPLES_PER_FRAME  NUM_SAMPLES_PER_FRAME
#define HRTF_NUM_TAPS           31
#define HRTF_MAX_DELAY_SAMPLES  42
#define HRTF_BUFLEN             (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)
#define HRTF_PARAM_SMOOTH_ALPHA 0.01f

typedef struct {
    int buf_pos;
    struct {
        float buf[HRTF_BUFLEN];
        float hrir_coeff_cur[HRTF_NUM_TAPS];
        float hrir_coeff_tar[HRTF_NUM_TAPS];
    } ch[2];
    float itd_cur;
    float itd_tar;
} HrtfFilter;

static inline void hrtf_filter_init(HrtfFilter *f)
{
    memset(f, 0, sizeof(*f));
}

static inline void hrtf_filter_clear_history(HrtfFilter *f)
{
    f->buf_pos = 0;
    memset(f->ch[0].buf, 0, sizeof(f->ch[0].buf));
    memset(f->ch[1].buf, 0, sizeof(f->ch[1].buf));
}

static inline void
hrtf_filter_set_target_params(HrtfFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS],
                              float itd)
{
    f->itd_tar =
        fmaxf(-HRTF_MAX_DELAY_SAMPLES, fminf(itd, HRTF_MAX_DELAY_SAMPLES));

    for (int ch = 0; ch < 2; ch++) {
        float *coeff = f->ch[ch].hrir_coeff_tar;
        memcpy(coeff, hrir_coeff[ch], sizeof(f->ch[ch].hrir_coeff_tar));

        // Normalize coefficients for unity filter gain
        float s = 0.0f;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            s += fabsf(coeff[k]);
        }
        if (s == 0.0f || s == 1.0f) {
            break;
        }
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            coeff[k] /= s;
        }
    }
}

/*
 * The filter's float arithmetic is spelled out as clang emitted it for the
 * plain-C form this file had before (aarch64, -ffp-contract=fast): the
 * parameter step and each tap's delay interpolation fuse into one
 * multiply-add, the vectorized taps 0-23 multiply and then add, and the
 * scalar tail (taps 24-30) fuses. Contraction is off below (clang honours
 * the pragma under build.sh's -ffp-contract=fast-honor-pragmas, LTO or
 * not), so a different vectorization cannot change that: the output stays
 * bit for bit what it was, which deterministic replays depend on. Other
 * compilers are deterministic per binary, not bit-equal to this one.
 */
#if defined(__aarch64__)
#define HRTF_MADD(a, b, c) fmaf((a), (b), (c))
#else
#define HRTF_MADD(a, b, c) ((a) * (b) + (c))
#endif
#define HRTF_FUSED_TAPS_FROM 24

static inline float hrtf_filter_smooth_param(float cur, float tar)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    // FIXME: Match hardware parameter transition
    return HRTF_MADD(HRTF_PARAM_SMOOTH_ALPHA, tar - cur, cur);
}

static inline void hrtf_filter_step_parameters(HrtfFilter *f)
{
    for (int ch = 0; ch < 2; ch++) {
        float *coeff_cur = f->ch[ch].hrir_coeff_cur;
        float *coeff_tar = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            coeff_cur[k] = hrtf_filter_smooth_param(coeff_cur[k], coeff_tar[k]);
        }
    }
    f->itd_cur = hrtf_filter_smooth_param(f->itd_cur, f->itd_tar);
}

static inline uint32_t hrtf_float_bits(float x)
{
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

/*
 * True when a parameter step would leave every coefficient and the ITD
 * bit for bit unchanged. The step depends only on the current and target
 * values, so the whole frame then runs on fixed parameters. This is the
 * usual state: the smoothing settles a few frames after a guest write.
 */
static inline bool hrtf_filter_params_settled(const HrtfFilter *f)
{
    uint32_t diff = 0;
    for (int ch = 0; ch < 2; ch++) {
        const float *coeff_cur = f->ch[ch].hrir_coeff_cur;
        const float *coeff_tar = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            float next = hrtf_filter_smooth_param(coeff_cur[k], coeff_tar[k]);
            diff |= hrtf_float_bits(coeff_cur[k]) ^ hrtf_float_bits(next);
        }
    }
    float next = hrtf_filter_smooth_param(f->itd_cur, f->itd_tar);
    diff |= hrtf_float_bits(f->itd_cur) ^ hrtf_float_bits(next);
    return diff == 0;
}

/* Interaural time difference (channel delay), whole and fractional. */
static inline void hrtf_filter_delay(const HrtfFilter *f, int ch, int *di,
                                     float *dfrac)
{
    float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
    if (d < 0.0f) {
        d = 0.0f;
    }
    *di = d;
    *dfrac = d - *di;
}

/*
 * A frame with settled parameters. A tap's delay interpolation depends only
 * on its input index, so it runs once per input sample instead of once per
 * tap and output; then each tap is applied to all outputs at once, every
 * output summing its taps in the same order as the per-sample loop, so the
 * result is identical.
 */
static inline void
hrtf_filter_convolve_settled(const HrtfFilter *f,
                             float lin[2][HRTF_BUFLEN + HRTF_SAMPLES_PER_FRAME],
                             float out[HRTF_SAMPLES_PER_FRAME][2])
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    float acc[2][HRTF_SAMPLES_PER_FRAME];

    for (int ch = 0; ch < 2; ch++) {
        const float *coeff = f->ch[ch].hrir_coeff_cur;
        int di;
        float dfrac;
        hrtf_filter_delay(f, ch, &di, &dfrac);
        float one_minus_dfrac = 1.0f - dfrac;

        /* s[j]: input j interpolated with the one before it (j >= 1). */
        float s[HRTF_BUFLEN + HRTF_SAMPLES_PER_FRAME];
        for (int j = 1; j < HRTF_BUFLEN + HRTF_SAMPLES_PER_FRAME; j++) {
            s[j] = HRTF_MADD(lin[ch][j], one_minus_dfrac,
                             lin[ch][j - 1] * dfrac);
        }

        /* base[n - k] is tap k of output n, as in the per-sample loop. */
        const float *base = &s[HRTF_BUFLEN - di];
#if defined(__aarch64__) && defined(__ARM_NEON)
        /*
         * Lanes run over outputs (clang vectorizes the C form over taps).
         * Plain operators, not vmulq/vaddq: those inline from arm_neon.h
         * with the global contraction setting and would fuse.
         */
        float32x4_t a[HRTF_SAMPLES_PER_FRAME / 4];
        for (int i = 0; i < HRTF_SAMPLES_PER_FRAME / 4; i++) {
            a[i] = vdupq_n_f32(0.0f);
        }
        for (int k = 0; k < HRTF_FUSED_TAPS_FROM; k++) {
            float32x4_t c = vdupq_n_f32(coeff[k]);
            for (int i = 0; i < HRTF_SAMPLES_PER_FRAME / 4; i++) {
                a[i] = a[i] + c * vld1q_f32(&base[4 * i - k]);
            }
        }
        for (int k = HRTF_FUSED_TAPS_FROM; k < HRTF_NUM_TAPS; k++) {
            float32x4_t c = vdupq_n_f32(coeff[k]);
            for (int i = 0; i < HRTF_SAMPLES_PER_FRAME / 4; i++) {
                a[i] = vfmaq_f32(a[i], c, vld1q_f32(&base[4 * i - k]));
            }
        }
        for (int i = 0; i < HRTF_SAMPLES_PER_FRAME / 4; i++) {
            vst1q_f32(&acc[ch][4 * i], a[i]);
        }
#else
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            acc[ch][n] = 0.0f;
        }
        for (int k = 0; k < HRTF_FUSED_TAPS_FROM; k++) {
            for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
                acc[ch][n] = acc[ch][n] + coeff[k] * base[n - k];
            }
        }
        for (int k = HRTF_FUSED_TAPS_FROM; k < HRTF_NUM_TAPS; k++) {
            for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
                acc[ch][n] = HRTF_MADD(coeff[k], base[n - k], acc[ch][n]);
            }
        }
#endif
    }

    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        out[n][0] = acc[0][n];
        out[n][1] = acc[1][n];
    }
}

static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[HRTF_SAMPLES_PER_FRAME][2],
                                       float out[HRTF_SAMPLES_PER_FRAME][2])
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    /*
     * Linearize the circular history once per frame (two-span copy)
     * and append the frame's input samples, so the 31-tap convolution
     * below runs over contiguous memory with no per-tap modulo. The
     * `% HRTF_BUFLEN` indexing in the previous per-sample form
     * defeated the autovectorizer and was the dominant cost for
     * 3D/HRTF voices. Frames with settled parameters then take the
     * hand-NEON pass over outputs above; the per-sample path is left to
     * the autovectorizer (an older gather-over-taps NEON lost to it).
     *
     * Layout: lin[ch][0 .. HRTF_BUFLEN-1] is history, oldest first;
     * lin[ch][HRTF_BUFLEN + n] is input sample n. Prefilling all
     * inputs up front also makes in == out aliasing (the common call
     * pattern) explicitly safe.
     */
    float lin[2][HRTF_BUFLEN + HRTF_SAMPLES_PER_FRAME];

    for (int ch = 0; ch < 2; ch++) {
        const float *buf = f->ch[ch].buf;
        int p = f->buf_pos;
        size_t tail = HRTF_BUFLEN - p;
        memcpy(&lin[ch][0], &buf[p], tail * sizeof(float));
        memcpy(&lin[ch][tail], &buf[0], p * sizeof(float));
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            lin[ch][HRTF_BUFLEN + n] = in[n][ch];
        }
    }

    if (hrtf_filter_params_settled(f)) {
        hrtf_filter_convolve_settled(f, lin, out);
    } else {
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            hrtf_filter_step_parameters(f);

            for (int ch = 0; ch < 2; ch++) {
                const float *coeff = f->ch[ch].hrir_coeff_cur;
                int di;
                float dfrac;
                hrtf_filter_delay(f, ch, &di, &dfrac);
                float one_minus_dfrac = 1.0f - dfrac;

                /*
                 * base points at the newest tap (k == 0). Indices stay in
                 * bounds: HRTF_BUFLEN(73) - di(<=42) - k(<=30) - 1 >= 0.
                 */
                const float *base = &lin[ch][HRTF_BUFLEN + n - di];

                // HRIR Convolution
                float acc = 0.0f;
                for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                    float s = HRTF_MADD(base[-k], one_minus_dfrac,
                                        base[-k - 1] * dfrac);
                    acc = k < HRTF_FUSED_TAPS_FROM ? acc + coeff[k] * s :
                                                     HRTF_MADD(coeff[k], s, acc);
                }

                out[n][ch] = acc;
            }
        }
    }

    /*
     * Store the last HRTF_BUFLEN samples back as canonical history
     * (oldest at index 0); buf_pos = 0 keeps the circular-buffer
     * invariant "newest sample at buf_pos - 1".
     */
    for (int ch = 0; ch < 2; ch++) {
        memcpy(f->ch[ch].buf, &lin[ch][HRTF_SAMPLES_PER_FRAME],
               HRTF_BUFLEN * sizeof(float));
    }
    f->buf_pos = 0;
}

#endif
