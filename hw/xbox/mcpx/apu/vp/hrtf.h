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

static inline float hrtf_filter_smooth_param(float cur, float tar)
{
    // FIXME: Match hardware parameter transition
    return cur + HRTF_PARAM_SMOOTH_ALPHA * (tar - cur);
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

static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[HRTF_SAMPLES_PER_FRAME][2],
                                       float out[HRTF_SAMPLES_PER_FRAME][2])
{
    /*
     * Linearize the circular history once per frame (two-span copy)
     * and append the frame's input samples, so the 31-tap convolution
     * below runs over contiguous memory with no per-tap modulo. The
     * `% HRTF_BUFLEN` indexing in the previous per-sample form
     * defeated the autovectorizer and was the dominant cost for
     * 3D/HRTF voices. (Hand-NEON was tried and reverted — see README
     * failed experiments — but removing the modulo lets -O3
     * autovectorize profitably.)
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

    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        hrtf_filter_step_parameters(f);

        for (int ch = 0; ch < 2; ch++) {
            const float *coeff = f->ch[ch].hrir_coeff_cur;

            // Interaural time difference (channel delay)
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) {
                d = 0.0f;
            }
            int di = d;
            float dfrac = d - di;
            float one_minus_dfrac = 1.0f - dfrac;

            /*
             * base points at the newest tap (k == 0). Indices stay in
             * bounds: HRTF_BUFLEN(73) - di(<=42) - k(<=30) - 1 >= 0.
             */
            const float *base = &lin[ch][HRTF_BUFLEN + n - di];

            // HRIR Convolution
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                float s = base[-k] * one_minus_dfrac + base[-k - 1] * dfrac;
                acc += coeff[k] * s;
            }

            out[n][ch] = acc;
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
