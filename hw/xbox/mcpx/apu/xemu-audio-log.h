/*
 * xemu: audio checks without a host device
 *
 * A per-voice event log (XEMU_VOICE_LOG, QMP xemu-voice-log-start/-stop)
 * and a WAV file of the APU's mixed output (XEMU_AUDIO_WAV, QMP
 * xemu-audio-wav-start/-stop). xemu-audio-log.c has the formats.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_MCPX_APU_XEMU_AUDIO_LOG_H
#define HW_XBOX_MCPX_APU_XEMU_AUDIO_LOG_H

typedef struct MCPXAPUState MCPXAPUState;

/* A log or WAV file is open. The hooks cost one branch each otherwise. */
extern bool xemu_voice_log_on;
extern bool xemu_audio_wav_on;

typedef enum XemuVoiceEvent {
    XEMU_VOICE_START,       /* VOICE_ON */
    XEMU_VOICE_RELEASE,     /* VOICE_RELEASE */
    XEMU_VOICE_PAUSE,       /* VOICE_PAUSE, action set */
    XEMU_VOICE_RESUME,      /* VOICE_PAUSE, action clear */
    XEMU_VOICE_STOP,        /* voice_off */
} XemuVoiceEvent;

typedef enum XemuVoiceWhy {
    XEMU_VOICE_WHY_NONE,
    XEMU_VOICE_WHY_GUEST,   /* the guest's VOICE_OFF */
    XEMU_VOICE_WHY_END,     /* a buffer without loop or a stream ran out */
    XEMU_VOICE_WHY_ENV,     /* the amplitude envelope finished its release */
    XEMU_VOICE_WHY_STREAM,  /* a stream without PERSIST outside release */
} XemuVoiceWhy;

/*
 * Called with d->lock held, after a VOICE_ON/RELEASE/PAUSE method took
 * effect, or at the top of voice_off. Rows for anything but a start are
 * written only while the voice is active.
 */
void xemu_voice_log_event(MCPXAPUState *d, uint16_t v, XemuVoiceEvent ev,
                          XemuVoiceWhy why);

static inline void xemu_voice_log(MCPXAPUState *d, uint16_t v,
                                  XemuVoiceEvent ev, XemuVoiceWhy why)
{
    if (unlikely(qatomic_read(&xemu_voice_log_on))) {
        xemu_voice_log_event(d, v, ev, why);
    }
}

/* count interleaved stereo S16 samples of output (one EP period: 256). */
void xemu_audio_wav_write(const int16_t *samples, int count);

/* XEMU_AUDIO_WAV was given: open no host audio device. */
bool xemu_audio_wav_from_env(void);

#endif
