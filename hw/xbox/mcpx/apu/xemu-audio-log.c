/*
 * xemu: audio checks without a host device
 *
 * Voice log: XEMU_VOICE_LOG=<file> (from launch) or QMP
 * xemu-voice-log-start/-stop write one tab-separated row per voice event,
 * under a header line:
 *
 *   frame guest_ns voice event why phys bytes fmt ch flags
 *   ba cbo lbo ebo pitch hz vol bins wav_pos runs
 *
 * event is start (VOICE_ON), release, pause, resume or stop; why says
 * what stopped a voice (guest: VOICE_OFF; end: a buffer without loop or a
 * stream ran out; env: the amplitude envelope finished its release;
 * stream: a stream voice without PERSIST outside its release, which the VP
 * ends at once). Rows other than start are written only for an active
 * voice; one already playing when the log opens has no start row. phys
 * is the physical address of the voice's first sample byte and bytes the
 * data's length: for a buffer voice BA through sample EBO, translated
 * through the VP's scatter-gather table; for a stream voice the SSL
 * segment it is on; "-" and 0 for a multipass voice or data outside RAM.
 * runs lists the data's physical extents in order as ADDR+LEN, merging
 * pages the table maps contiguously (at most 256, then "...").
 * fmt is u8, s16, s24, s32 or adpcm, ch 1 or 2, flags l(oop) s(tream)
 * p(ersist) m(ultipass) or "-". ba is the buffer's offset in the
 * scatter-gather space; cbo, lbo and ebo the current, loop and end sample
 * offsets. pitch is the raw signed pitch (1/4096 octave from 48 kHz) and
 * hz the sample rate it gives. vol lists the 8 volume slots as 12-bit hex
 * attenuations (1/64 dB; fff is silent) and bins their mixbins (slots 0-3
 * of a 3D voice, 0-63, are the HRTF submixes). wav_pos is the WAV sample
 * the next APU frame lands on while a WAV is open, else -1. frame is
 * query-xemu-frame's counter and guest_ns QEMU_CLOCK_VIRTUAL.
 *
 * WAV: XEMU_AUDIO_WAV=<file> (from launch, and xemu then opens no host
 * audio device) or QMP xemu-audio-wav-start/-stop write what the APU
 * outputs (the samples it would play: the EP's output, or the GP's or VP's
 * when the EP is off) as 48 kHz stereo S16 PCM, one EP period (256
 * samples) at a time, without volume_limit applied. Periods the APU does
 * not run (VM paused, APU gated) leave no gap.
 *
 * Both run where the APU does: the frame thread and the voice workers in
 * real time, the vCPU thread (BQL held) under icount, where the files are
 * as deterministic as the RAM. Neither changes what the guest sees, and
 * both are flushed when the VM stops, when they stop, and at exit; the
 * voice log also at a row 250 ms or more after its last flush (its file
 * plumbing is ui/xemu-filelog.c's), the WAV (header included) every
 * second of audio.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "system/runstate.h"
#include "ui/xemu-filelog.h"
#include "apu_int.h"
#include "xemu-audio-log.h"

#define VOICE_LOG_HEADER \
    "frame\tguest_ns\tvoice\tevent\twhy\tphys\tbytes\tfmt\tch\tflags\t" \
    "ba\tcbo\tlbo\tebo\tpitch\thz\tvol\tbins\twav_pos\truns\n"
#define VOICE_LOG_MAX_RUNS 256
#define WAV_RATE 48000
#define WAV_HEADER_BYTES 44
#define WAV_FLUSH_SAMPLES WAV_RATE

bool xemu_voice_log_on;
bool xemu_audio_wav_on;

static void voice_log_before_close(void);

static QemuMutex log_lock;      /* the voice log; a leaf lock */
static XemuFileLog voice_log = {
    .what = "voice log",
    .header = VOICE_LOG_HEADER,
    .lock = &log_lock,
    .before_close = voice_log_before_close,
};

static QemuMutex wav_lock;      /* the WAV file; a leaf lock */
static FILE *wav_file;
static char *wav_path;
static int64_t wav_samples;     /* stereo samples written (atomic) */
static int64_t wav_unflushed;

static bool wav_hooks_registered;

static const char *const event_names[] = {
    [XEMU_VOICE_START] = "start",
    [XEMU_VOICE_RELEASE] = "release",
    [XEMU_VOICE_PAUSE] = "pause",
    [XEMU_VOICE_RESUME] = "resume",
    [XEMU_VOICE_STOP] = "stop",
};

static const char *const why_names[] = {
    [XEMU_VOICE_WHY_NONE] = "-",
    [XEMU_VOICE_WHY_GUEST] = "guest",
    [XEMU_VOICE_WHY_END] = "end",
    [XEMU_VOICE_WHY_ENV] = "env",
    [XEMU_VOICE_WHY_STREAM] = "stream",
};

/* Guest RAM only: past it the memory API could reach MMIO. */
static bool ram_word(MCPXAPUState *d, hwaddr addr, uint32_t *out)
{
    if (addr > d->ram_size || d->ram_size - addr < 4) {
        return false;
    }
    *out = ldl_le_p(&d->ram_ptr[addr]);
    return true;
}

/* As the VP's get_data_ptr: a scatter-gather offset to a physical address. */
static int64_t sge_phys(MCPXAPUState *d, uint32_t offset)
{
    hwaddr entry = qatomic_read(&d->regs[NV_PAPU_VPSGEADDR]) +
                   (hwaddr)(offset / TARGET_PAGE_SIZE) * 8;
    uint32_t page;

    if (!ram_word(d, entry, &page)) {
        return -1;
    }
    return (uint32_t)(page + offset % TARGET_PAGE_SIZE);
}

static void put_run(GString *runs, int64_t addr, int64_t len)
{
    g_string_append_printf(runs, "%s0x%08" PRIx64 "+%" PRId64,
                           runs->len ? "," : "", addr, len);
}

/*
 * The physical extents of a buffer voice's data, in order, merging pages
 * the scatter-gather table maps contiguously. Returns the first byte's
 * address, or -1 when the table or the data is outside RAM.
 */
static int64_t sge_runs(MCPXAPUState *d, uint32_t ba, int64_t bytes,
                        GString *runs)
{
    int64_t first = -1, start = -1, len = 0;
    int n = 0;

    while (bytes > 0) {
        int64_t chunk = MIN(bytes, TARGET_PAGE_SIZE - ba % TARGET_PAGE_SIZE);
        int64_t addr = sge_phys(d, ba);

        if (addr < 0 || addr + chunk > d->ram_size) {
            if (start >= 0) {
                put_run(runs, start, len);
            }
            g_string_append(runs, runs->len ? ",?" : "?");
            return first;
        }
        if (first < 0) {
            first = addr;
        }
        if (start >= 0 && addr == start + len) {
            len += chunk;
        } else {
            if (start >= 0) {
                put_run(runs, start, len);
                if (++n == VOICE_LOG_MAX_RUNS) {
                    g_string_append(runs, ",...");
                    return first;
                }
            }
            start = addr;
            len = chunk;
        }
        ba += chunk;
        bytes -= chunk;
    }
    if (start >= 0) {
        put_run(runs, start, len);
    }
    return first;
}

static inline uint32_t voice_field(uint32_t word, uint32_t mask)
{
    return (word & mask) >> ctz32(mask);
}

void xemu_voice_log_event(MCPXAPUState *d, uint16_t v, XemuVoiceEvent ev,
                          XemuVoiceWhy why)
{
    static const char *const sizes[] = { "u8", "s16", "s24", "s32" };
    static const unsigned containers[] = { 1, 2, 0, 4 };
    uint32_t w[NV_PAVS_SIZE / 4] = { 0 };
    int64_t phys = -1, bytes = 0, wav_pos = -1;
    char flags[8], *fp = flags;
    GString *row, *runs;
    hwaddr base;

    if (v >= MCPX_HW_MAX_VOICES) {
        return;
    }
    base = qatomic_read(&d->regs[NV_PAPU_VPVADDR]) + (hwaddr)v * NV_PAVS_SIZE;
    for (int i = 0; i < ARRAY_SIZE(w); i++) {
        ram_word(d, base + i * 4, &w[i]);
    }
#define W(reg) (w[(reg) / 4])
#define F(reg, mask) voice_field(W(reg), mask)
    if (ev != XEMU_VOICE_START &&
        !F(NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
        return;
    }

    uint32_t fmt = W(NV_PAVS_VOICE_CFG_FMT);
    bool stereo = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned sample_size = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned container = voice_field(fmt,
                                     NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    bool adpcm = container == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    bool stream = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool loop = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_LOOP);
    bool persist = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_PERSIST);
    bool multipass = voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    unsigned spb = 1 + voice_field(fmt,
                                   NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    uint32_t ba = F(NV_PAVS_VOICE_CUR_PSL_START,
                    NV_PAVS_VOICE_CUR_PSL_START_BA);
    uint32_t cbo = F(NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = F(NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                     NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ebo = F(NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO);
    int16_t pitch = F(NV_PAVS_VOICE_TAR_PITCH_LINK,
                      NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);

    /* The data the VP reads (voice_get_samples). */
    runs = g_string_sized_new(64);
    if (multipass) {
        /* reads a mixbin, no buffer */
    } else if (stream) {
        MCPXAPUVPSSLData *ssl = &d->vp.ssl[v];
        int idx = ssl->ssl_index;
        uint32_t off, len;

        if (idx >= 0 && idx < MCPX_HW_SSLS_PER_VOICE &&
            ssl->ssl_seg >= 0 && ssl->ssl_seg < ssl->count[idx]) {
            hwaddr entry = qatomic_read(&d->regs[NV_PAPU_VPSSLADDR]) +
                           (hwaddr)(ssl->base[idx] + ssl->ssl_seg) * 8;
            if (ram_word(d, entry, &off) && ram_word(d, entry + 4, &len)) {
                unsigned seg_len = len & 0xffff;
                unsigned seg_cs = (len >> 16) & 3;
                unsigned seg_spb = ((len >> 18) & 0x1f) + 1;
                if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
                    bytes = (int64_t)(seg_len >> 6) * 36 * seg_spb;
                } else {
                    bytes = (int64_t)seg_len * containers[seg_cs] * seg_spb;
                }
                if (bytes && off + bytes <= d->ram_size) {
                    phys = off;
                    put_run(runs, off, bytes);
                }
            }
        }
    } else {
        int64_t block = adpcm ? 36 * spb : containers[container] * spb;
        bytes = adpcm ? (int64_t)(ebo / ADPCM_SAMPLES_PER_BLOCK + 1) * block
                      : (int64_t)(ebo + 1) * block;
        phys = sge_runs(d, ba, bytes, runs);
    }
    if (phys < 0) {
        bytes = 0;
    }

    uint32_t vola = W(NV_PAVS_VOICE_TAR_VOLA);
    uint32_t volb = W(NV_PAVS_VOICE_TAR_VOLB);
    uint32_t volc = W(NV_PAVS_VOICE_TAR_VOLC);
    uint32_t vbin = W(NV_PAVS_VOICE_CFG_VBIN);
    unsigned vol[8] = {
        voice_field(vola, NV_PAVS_VOICE_TAR_VOLA_VOLUME0),
        voice_field(vola, NV_PAVS_VOICE_TAR_VOLA_VOLUME1),
        voice_field(volb, NV_PAVS_VOICE_TAR_VOLB_VOLUME2),
        voice_field(volb, NV_PAVS_VOICE_TAR_VOLB_VOLUME3),
        voice_field(volc, NV_PAVS_VOICE_TAR_VOLC_VOLUME4),
        voice_field(volc, NV_PAVS_VOICE_TAR_VOLC_VOLUME5),
        voice_field(volc, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8 |
            voice_field(volb, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4 |
            voice_field(vola, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0),
        voice_field(volc, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8 |
            voice_field(volb, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4 |
            voice_field(vola, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0),
    };
    unsigned bin[8] = {
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V0BIN),
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V1BIN),
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V2BIN),
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V3BIN),
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V4BIN),
        voice_field(vbin, NV_PAVS_VOICE_CFG_VBIN_V5BIN),
        voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_V6BIN),
        voice_field(fmt, NV_PAVS_VOICE_CFG_FMT_V7BIN),
    };
    if (v < MCPX_HW_MAX_3D_VOICES) {
        for (int i = 0; i < 4; i++) {
            bin[i] = d->vp.hrtf_submix[i];
        }
    }
#undef F
#undef W

    if (loop) {
        *fp++ = 'l';
    }
    if (stream) {
        *fp++ = 's';
    }
    if (persist) {
        *fp++ = 'p';
    }
    if (multipass) {
        *fp++ = 'm';
    }
    if (fp == flags) {
        *fp++ = '-';
    }
    *fp = 0;

    if (qatomic_read(&xemu_audio_wav_on)) {
        wav_pos = qatomic_read(&wav_samples) +
                  (int64_t)(d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
    }

    row = g_string_sized_new(256 + runs->len);
    g_string_append_printf(row, "%" PRId64 "\t%" PRId64 "\t%u\t%s\t%s",
                           xemu_frame_count(),
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), (unsigned)v,
                           event_names[ev], why_names[why]);
    if (phys < 0) {
        g_string_append(row, "\t-");
    } else {
        g_string_append_printf(row, "\t0x%08" PRIx64, phys);
    }
    g_string_append_printf(row, "\t%" PRId64 "\t%s\t%d\t%s\t0x%06x\t%u\t%u"
                           "\t%u\t%d\t%.0f\t", bytes,
                           adpcm ? "adpcm" : sizes[sample_size],
                           stereo ? 2 : 1, flags, ba, cbo, lbo, ebo, pitch,
                           WAV_RATE * exp2(pitch / 4096.0));
    for (int i = 0; i < 8; i++) {
        g_string_append_printf(row, i ? ",%03x" : "%03x", vol[i]);
    }
    g_string_append_c(row, '\t');
    for (int i = 0; i < 8; i++) {
        g_string_append_printf(row, i ? ",%u" : "%u", bin[i]);
    }
    g_string_append_printf(row, "\t%" PRId64 "\t%s\n", wav_pos,
                           runs->len ? runs->str : "-");
    g_string_free(runs, true);

    xemu_filelog_printf(&voice_log, "%s", row->str);
    g_string_free(row, true);
}

static void wav_put_header(FILE *f, int64_t samples)
{
    uint64_t data = MIN((uint64_t)samples * 4, UINT32_MAX - 36);
    uint8_t h[WAV_HEADER_BYTES];

    memcpy(h, "RIFF", 4);
    stl_le_p(h + 4, data + 36);
    memcpy(h + 8, "WAVEfmt ", 8);
    stl_le_p(h + 16, 16);                   /* fmt chunk size */
    stw_le_p(h + 20, 1);                    /* PCM */
    stw_le_p(h + 22, 2);                    /* channels */
    stl_le_p(h + 24, WAV_RATE);
    stl_le_p(h + 28, WAV_RATE * 4);         /* bytes per second */
    stw_le_p(h + 32, 4);                    /* bytes per sample frame */
    stw_le_p(h + 34, 16);                   /* bits per sample */
    memcpy(h + 36, "data", 4);
    stl_le_p(h + 40, data);
    fwrite(h, 1, sizeof(h), f);
}

/* Write the sizes into the header and flush; wav_lock held. */
static void wav_sync(void)
{
    if (fseek(wav_file, 0, SEEK_SET) == 0) {
        wav_put_header(wav_file, wav_samples);
        fseek(wav_file, 0, SEEK_END);
    }
    fflush(wav_file);
    wav_unflushed = 0;
}

void xemu_audio_wav_write(const int16_t *samples, int count)
{
    qemu_mutex_lock(&wav_lock);
    if (wav_file) {
#if HOST_BIG_ENDIAN
        int16_t le[512];
        for (int i = 0; i < count * 2; i++) {
            le[i % 512] = cpu_to_le16(samples[i]);
            if (i % 512 == 511 || i == count * 2 - 1) {
                fwrite(le, 2, i % 512 + 1, wav_file);
            }
        }
#else
        fwrite(samples, 4, count, wav_file);
#endif
        qatomic_set(&wav_samples, wav_samples + count);
        wav_unflushed += count;
        if (wav_unflushed >= WAV_FLUSH_SAMPLES) {
            wav_sync();
        }
    }
    qemu_mutex_unlock(&wav_lock);
}

bool xemu_audio_wav_from_env(void)
{
    const char *path = getenv("XEMU_AUDIO_WAV");

    return path && path[0];
}

static void wav_vm_state(void *opaque, bool running, RunState state)
{
    if (running) {
        return;
    }
    qemu_mutex_lock(&wav_lock);
    if (wav_file) {
        wav_sync();
    }
    qemu_mutex_unlock(&wav_lock);
}

static void voice_log_before_close(void)
{
    qatomic_set(&xemu_voice_log_on, false);
}

static bool wav_close(Error **errp)
{
    bool ok;

    qemu_mutex_lock(&wav_lock);
    qatomic_set(&xemu_audio_wav_on, false);
    wav_sync();
    ok = !ferror(wav_file);
    ok &= fclose(wav_file) == 0;
    wav_file = NULL;
    qemu_mutex_unlock(&wav_lock);
    if (!ok) {
        error_setg(errp, "writing WAV '%s' failed", wav_path);
    }
    return ok;
}

static void wav_exit(void)
{
    if (wav_file) {
        wav_close(NULL);
    }
}

static bool voice_log_open(const char *path, bool append, Error **errp)
{
    if (!xemu_filelog_open(&voice_log, path, append, errp)) {
        return false;
    }
    qatomic_set(&xemu_voice_log_on, true);
    return true;
}

static bool wav_open(const char *path, Error **errp)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        error_setg_errno(errp, errno, "can't open WAV '%s'", path);
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    wav_put_header(f, 0);
    if (!wav_hooks_registered) {
        qemu_add_vm_change_state_handler(wav_vm_state, NULL);
        atexit(wav_exit);
        wav_hooks_registered = true;
    }
    qemu_mutex_lock(&wav_lock);
    g_free(wav_path);
    wav_path = g_strdup(path);
    wav_file = f;
    qatomic_set(&wav_samples, 0);
    wav_unflushed = 0;
    qatomic_set(&xemu_audio_wav_on, true);
    qemu_mutex_unlock(&wav_lock);
    return true;
}

void qmp_xemu_voice_log_start(const char *filename, bool has_append,
                              bool append, Error **errp)
{
    voice_log_open(filename, has_append && append, errp);
}

XemuVoiceLogInfo *qmp_xemu_voice_log_stop(Error **errp)
{
    XemuVoiceLogInfo *info;

    if (!xemu_filelog_close(&voice_log, errp)) {
        return NULL;
    }
    info = g_new0(XemuVoiceLogInfo, 1);
    info->filename = g_strdup(voice_log.path);
    info->rows = voice_log.rows;
    return info;
}

void qmp_xemu_audio_wav_start(const char *filename, Error **errp)
{
    if (wav_file) {
        error_setg(errp, "a WAV is already open: '%s'", wav_path);
        return;
    }
    wav_open(filename, errp);
}

XemuAudioWavInfo *qmp_xemu_audio_wav_stop(Error **errp)
{
    XemuAudioWavInfo *info;

    if (!wav_file) {
        error_setg(errp, "no WAV is open");
        return NULL;
    }
    if (!wav_close(errp)) {
        return NULL;
    }
    info = g_new0(XemuAudioWavInfo, 1);
    info->filename = g_strdup(wav_path);
    info->samples = wav_samples;
    return info;
}

static void audio_log_env_init(void)
{
    const char *log = getenv("XEMU_VOICE_LOG");
    const char *wav = getenv("XEMU_AUDIO_WAV");
    Error *err = NULL;

    qemu_mutex_init(&log_lock);
    qemu_mutex_init(&wav_lock);
    if (log && log[0] && !voice_log_open(log, false, &err)) {
        error_report_err(err);
        err = NULL;
    }
    if (wav && wav[0]) {
        if (wav_open(wav, &err)) {
            fprintf(stderr, "xemu: audio WAV: %s (no host audio device)\n",
                    wav);
        } else {
            error_report_err(err);
        }
    }
}
opts_init(audio_log_env_init);
