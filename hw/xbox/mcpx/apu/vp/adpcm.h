/*
 * ADPCM decoder from the ADPCM-XQ project: https://github.com/dbry/adpcm-xq
 *
 *                        Copyright (c) David Bryant
 *                           All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Conifer Software nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ADPCM_DECODE_H
#define ADPCM_DECODE_H

/********************************* 4-bit ADPCM decoder ********************************/

#define CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

/* step table */
static const uint16_t adpcm_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,
    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

/* step index tables */
static const int adpcm_index_table[] = {
    /* adpcm data size is 4 */
    -1, -1, -1, -1, 2, 4, 6, 8
};

/* One 4-bit code of one channel; returns the new sample. */
static inline __attribute__((always_inline)) int16_t
adpcm_decode_nibble(int32_t *pcmdata, int *index, unsigned int nibble)
{
    int step = adpcm_step_table[*index], delta = step >> 3;

    if (nibble & 1) delta += (step >> 2);
    if (nibble & 2) delta += (step >> 1);
    if (nibble & 4) delta += step;
    if (nibble & 8) delta = -delta;

    *pcmdata += delta;
    *index += adpcm_index_table[nibble & 0x7];
    CLIP(*index, 0, 88);
    CLIP(*pcmdata, -32768, 32767);
    return *pcmdata;
}

/* One channel's header; false if it is malformed. */
static inline __attribute__((always_inline)) bool
adpcm_decode_header(int16_t *outbuf, const uint8_t *inbuf, int32_t *pcmdata,
                    int *index)
{
    *outbuf = *pcmdata = (int16_t) (inbuf [0] | (inbuf [1] << 8));
    *index = (int8_t) inbuf [2];

    return !(*index < 0 || *index > 88 || inbuf [3]);  // sanitize the input a little...
}

/*
 * adpcm_decode_block for a constant channel count, with each channel's
 * state in locals rather than in arrays indexed by channel.
 */
static inline __attribute__((always_inline)) int
adpcm_decode_block_n(int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize,
                     const int channels)
{
    int samples = 1, chunks;
    int32_t pcm0, pcm1 = 0;
    int index0, index1 = 0;

    if (inbufsize < (uint32_t) channels * 4)
        return 0;

    if (!adpcm_decode_header(outbuf++, inbuf, &pcm0, &index0))
        return 0;
    inbufsize -= 4;
    inbuf += 4;
    if (channels == 2) {
        if (!adpcm_decode_header(outbuf++, inbuf, &pcm1, &index1))
            return 0;
        inbufsize -= 4;
        inbuf += 4;
    }

    chunks = inbufsize / (channels * 4);
    samples += chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            outbuf [i * 2 * channels] =
                adpcm_decode_nibble(&pcm0, &index0, inbuf [i] & 0xf);
            outbuf [(i * 2 + 1) * channels] =
                adpcm_decode_nibble(&pcm0, &index0, inbuf [i] >> 4);
        }
        inbuf += 4;

        if (channels == 2) {
            for (int i = 0; i < 4; ++i) {
                outbuf [i * 2 * channels + 1] =
                    adpcm_decode_nibble(&pcm1, &index1, inbuf [i] & 0xf);
                outbuf [(i * 2 + 1) * channels + 1] =
                    adpcm_decode_nibble(&pcm1, &index1, inbuf [i] >> 4);
            }
            inbuf += 4;
        }

        outbuf += channels * 8;
    }

    return samples;
}

/* Decode the block of ADPCM data into PCM. This requires no context because ADPCM blocks
 * are indeppendently decodable. This assumes that a single entire block is always decoded;
 * it must be called multiple times for multiple blocks and cannot resume in the middle of a
 * block.
 *
 * Parameters:
 *  outbuf          destination for interleaved PCM samples
 *  inbuf           source ADPCM block
 *  inbufsize       size of source ADPCM block
 *  channels        number of channels in block (must be determined from other context)
 *
 * Returns number of converted composite samples (total samples divided by number of channels)
 */

static int adpcm_decode_block (int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels)
{
    if (channels == 2) {
        return adpcm_decode_block_n(outbuf, inbuf, inbufsize, 2);
    }
    assert(channels == 1);
    return adpcm_decode_block_n(outbuf, inbuf, inbufsize, 1);
}

#undef CLIP

#endif
