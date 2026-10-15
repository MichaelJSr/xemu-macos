/*
 * Linear-interpolating sample-rate converter, libsamplerate 0.2.2's
 * SRC_LINEAR in callback mode, with its state in plain fields so that a
 * savestate can carry it.
 *
 * Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
 * Copyright (c) 2026 xemu-macos contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * src_callback_read(), src_process() and linear_vari_process() of
 * libsamplerate 0.2.2, folded into one function and limited to what the
 * voice processor uses (1 or 2 channels, a callback that always returns
 * input). The arithmetic is kept operation for operation, so the output
 * is the library's bit for bit. On aarch64 the library's build fuses three
 * expressions into multiply-adds (the interpolation and the two bounds
 * checks); this file fuses those three and nothing else.
 */

#ifndef HW_XBOX_MCPX_APU_VP_LINEAR_SRC_H
#define HW_XBOX_MCPX_APU_VP_LINEAR_SRC_H

#include <math.h>

#define LINEAR_SRC_MAX_RATIO 256

typedef struct LinearSrc {
    int32_t channels;           /* 0: none yet */
    bool dirty;                 /* last_value holds input */
    float last_value[2];
    double last_ratio;
    double last_position;
    const float *saved_data;    /* unread input, in the callback's buffer */
    int64_t saved_frames;
    int32_t saved_offset;       /* saved_data as an index, for savestates */
} LinearSrc;

typedef long (*LinearSrcCallback)(void *opaque, float **data);

#if defined(__aarch64__)
#define LINEAR_SRC_MADD(a, b, c) fma((a), (b), (c))
#else
#define LINEAR_SRC_MADD(a, b, c) ((a) * (b) + (c))
#endif

static inline void linear_src_reset(LinearSrc *s, int channels)
{
    memset(s, 0, sizeof(*s));
    s->channels = channels;
}

static inline bool linear_src_bad_ratio(double ratio)
{
    return ratio < (1.0 / LINEAR_SRC_MAX_RATIO) ||
           ratio > (1.0 * LINEAR_SRC_MAX_RATIO);
}

static inline double linear_src_fmod_one(double x)
{
    double res = x - lrint(x);
    if (res < 0.0) {
        return res + 1.0;
    }
    return res;
}

/*
 * libsamplerate's linear_vari_process, for one src_process() call; false
 * on its one error (a ratio gone bad, which a valid target never makes).
 */
static inline bool linear_src_process(LinearSrc *s, const float *in,
                                      long in_frames, float *out,
                                      long out_frames, double target,
                                      long *in_used_frames,
                                      long *out_gen_frames)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const int channels = s->channels;
    long in_count, in_used, out_count, out_gen;
    double src_ratio, input_index, rem;

    *in_used_frames = *out_gen_frames = 0;
    if (s->last_ratio < (1.0 / LINEAR_SRC_MAX_RATIO)) {
        s->last_ratio = target;
    }
    if (in_frames <= 0) {
        return true;
    }
    if (!s->dirty) {
        for (int ch = 0; ch < channels; ch++) {
            s->last_value[ch] = in[ch];
        }
        s->dirty = true;
    }

    in_count = in_frames * channels;
    out_count = out_frames * channels;
    in_used = out_gen = 0;

    src_ratio = s->last_ratio;
    if (linear_src_bad_ratio(src_ratio)) {
        return false;
    }
    input_index = s->last_position;

    /* Samples before the first one of the input. */
    while (input_index < 1.0 && out_gen < out_count) {
        if (LINEAR_SRC_MADD((double)channels, 1.0 + input_index,
                            (double)in_used) >= in_count) {
            break;
        }
        if (out_count > 0 &&
            fabs(s->last_ratio - target) > 1e-20) {
            src_ratio = s->last_ratio +
                        out_gen * (target - s->last_ratio) / out_count;
        }
        for (int ch = 0; ch < channels; ch++) {
            double last = s->last_value[ch];
            out[out_gen] = (float)LINEAR_SRC_MADD(input_index,
                                                  (double)in[ch] - last,
                                                  last);
            out_gen++;
        }
        input_index += 1.0 / src_ratio;
    }

    rem = linear_src_fmod_one(input_index);
    in_used += channels * lrint(input_index - rem);
    input_index = rem;

    while (out_gen < out_count &&
           LINEAR_SRC_MADD((double)channels, input_index,
                           (double)in_used) < in_count) {
        if (out_count > 0 &&
            fabs(s->last_ratio - target) > 1e-20) {
            src_ratio = s->last_ratio +
                        out_gen * (target - s->last_ratio) / out_count;
        }
        for (int ch = 0; ch < channels; ch++) {
            double prev = in[in_used - channels + ch];
            out[out_gen] = (float)LINEAR_SRC_MADD(input_index,
                                                  (double)in[in_used + ch] -
                                                  prev, prev);
            out_gen++;
        }
        input_index += 1.0 / src_ratio;
        rem = linear_src_fmod_one(input_index);
        in_used += channels * lrint(input_index - rem);
        input_index = rem;
    }

    if (in_used > in_count) {
        input_index += (in_used - in_count) / channels;
        in_used = in_count;
    }
    s->last_position = input_index;
    if (in_used > 0) {
        for (int ch = 0; ch < channels; ch++) {
            s->last_value[ch] = in[in_used - channels + ch];
        }
    }
    s->last_ratio = src_ratio;

    *in_used_frames = in_used / channels;
    *out_gen_frames = out_gen / channels;
    return true;
}

/* libsamplerate's src_callback_read: @frames frames of output into @out. */
static inline long linear_src_callback_read(LinearSrc *s,
                                            LinearSrcCallback cb,
                                            void *opaque, double ratio,
                                            long frames, float *out)
{
    const float *in = s->saved_data;
    long in_frames = s->saved_frames;
    long gen = 0;

    if (frames <= 0 || linear_src_bad_ratio(ratio)) {
        return 0;
    }
    while (gen < frames) {
        float dummy[1];
        bool end_of_input = false;
        long used, made;

        if (in_frames == 0) {
            float *ptr = dummy;
            in_frames = cb(opaque, &ptr);
            in = ptr;
            end_of_input = in_frames == 0;
        }
        if (!linear_src_process(s, in, in_frames, out, frames - gen, ratio,
                                &used, &made)) {
            s->saved_data = in;
            s->saved_frames = in_frames;
            return 0;
        }
        in += used * s->channels;
        in_frames -= used;
        out += made * s->channels;
        gen += made;
        if (end_of_input && made == 0) {
            break;
        }
    }
    s->saved_data = in;
    s->saved_frames = in_frames;
    return gen;
}

#endif
