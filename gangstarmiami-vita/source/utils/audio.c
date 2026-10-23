/*
 * See audio.h for the why. Implementation notes:
 * - Decoding via libvorbisfile (libvorbisfile.a + libvorbis.a + libogg.a
 *   ship with vitasdk). Files are opened by Vita path directly
 *   (GAME_DATA_PATH "<name>") so no io.c translation is involved.
 * - Resampling to the 48 kHz output rate is linear interpolation; SFX are
 *   resampled once at load, streams per chunk in the mixer thread.
 * - The loader decodes SFX and constructs stable stream decoders; only the
 *   mixer advances published streams. JNI commands and publication share a
 *   mutex. Output alternates two aligned buffers.
 * - Float args arriving through FalsoJNI varargs are read as double
 *   (C promotion) with NaN/range guards so a misread can only affect
 *   loudness, never indexing or state.
 */

#include "utils/audio.h"
#include "utils/utils.h"
#include "utils/logger.h"
#include "sound_files.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vorbis/vorbisfile.h>

#define OUT_RATE 48000
#define OUT_FRAMES 2048
#define MAX_SFX_VOICES 8
#define MAX_BIG_VOICES 4
#define SFX_CACHE_MAX 128
#define SFX_DECODE_CAP (2 * 1024 * 1024)

static int audio_port = -1;
static volatile int audio_running = 0;
static SceUID audio_mutex = -1;
static SceUID audio_thread = -1;
static SceUID audio_loader_thread = -1, audio_load_sema = -1;
static int audio_blocked = 0;
static float gain_music = 1.0f, gain_sfx = 1.0f, gain_vfx = 1.0f;
static SceUID audio_api_thread = -1;
static SceUInt64 api_wait_us, api_wait_max_us;
static unsigned api_lock_calls;

static void audio_lock(void) {
    int measured = sceKernelGetThreadId() == audio_api_thread;
    SceUInt64 started = measured ? sceKernelGetProcessTimeWide() : 0;
    sceKernelLockMutex(audio_mutex, 1, NULL);
    if (measured) {
        SceUInt64 elapsed = sceKernelGetProcessTimeWide() - started;
        api_wait_us += elapsed;
        if (elapsed > api_wait_max_us) api_wait_max_us = elapsed;
        api_lock_calls++;
    }
}

void audio_report_frame_stats(int frame) {
    if (api_wait_us >= 100000)
        l_note("[PERF] frame=%d audio_locks=%u wait_ms=%llu max_wait_ms=%llu",
               frame, api_lock_calls, (unsigned long long)(api_wait_us / 1000),
               (unsigned long long)(api_wait_max_us / 1000));
    api_wait_us = api_wait_max_us = 0;
    api_lock_calls = 0;
}


static float sane_vol(float v) {
    if (!(v >= 0.0f) || !(v <= 2.0f))
        return 1.0f;
    return v;
}

static float sane_pitch(float p) {
    if (!(p >= 0.25f) || !(p <= 4.0f))
        return 1.0f;
    return p;
}

static int snd_category(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 1;
    const char *n = gmv_sound_files[index];
    if (n[0] == 'm' && n[1] == '_')
        return 0;
    if (!strncmp(n, "sfx_", 4))
        return 1;
    return 2;
}

static int snd_is_effect(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 0;
    const char *name = gmv_sound_files[index];
    return !strncmp(name, "sfx_", 4) || !strncmp(name, "vfx_", 4);
}

static float cat_gain(int index) {
    switch (snd_category(index)) {
    case 0: return gain_music;
    case 1: return gain_sfx;
    default: return gain_vfx;
    }
}

static void snd_path(int index, char *out, size_t n) {
    if (index < 0 || index >= GMV_SOUND_COUNT) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%s%s", GAME_DATA_PATH, gmv_sound_files[index]);
}

static int8_t sound_exists_cache[GMV_SOUND_COUNT];

static int snd_exists(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 0;
    if (sound_exists_cache[index] == 0) {
        char path[512];
        snd_path(index, path, sizeof(path));
        sound_exists_cache[index] = file_exists(path) ? 1 : -1;
    }
    return (sound_exists_cache[index] == 1);
}

/* ---------------- SFX cache (fully decoded) ---------------- */

typedef struct {
    int index;
    int16_t *pcm;      /* stereo, OUT_RATE */
    uint32_t frames;
    int used;
    uint32_t last_used;
} sfx_entry_t;

static sfx_entry_t sfx_cache[SFX_CACHE_MAX];
static uint32_t sfx_tick = 0;

typedef struct {
    int active;
    int paused;
    sfx_entry_t *e;
    uint64_t pos_fix;  /* 48.16: decoded effects can exceed 65536 frames. */
    uint32_t step_fix;
    float vol;
} sfx_voice_t;

static sfx_voice_t sfx_voices[MAX_SFX_VOICES];

static sfx_entry_t *sfx_find(int index) {
    for (int i = 0; i < SFX_CACHE_MAX; i++)
        if (sfx_cache[i].used && sfx_cache[i].index == index) {
            sfx_cache[i].last_used = ++sfx_tick;
            return &sfx_cache[i];
        }
    return NULL;
}

/* Loader-only full decode into private PCM; publication happens under
 * audio_mutex after checking the request cancellation generation. */
static int sfx_decode_raw(int index, int16_t **out_pcm, uint32_t *out_frames_p) {
    char path[512];
    snd_path(index, path, sizeof(path));
    if (!path[0])
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) {
        fclose(f);
        return -1;
    }
    vorbis_info *vi = ov_info(&vf, -1);
    long src_rate = vi ? vi->rate : 44100;
    int src_ch = vi ? vi->channels : 1;
    double total = ov_time_total(&vf, -1);
    if (!(total > 0.0) || total > 30.0) {
        ov_clear(&vf);
        return -1;
    }
    uint32_t out_frames = (uint32_t)(total * OUT_RATE) + 1;
    if ((uint64_t)out_frames * 4 > SFX_DECODE_CAP) {
        ov_clear(&vf);
        return -1;
    }
    int16_t *pcm = calloc(out_frames, 4);
    if (!pcm) {
        ov_clear(&vf);
        return -1;
    }

    /* Decode at source rate into a stereo temp buffer, then
     * linear-resample to OUT_RATE. Chunked: mono files are upmixed to
     * stereo here, so stride bugs can't leak heap garbage into the mix. */
    uint32_t src_cap = (uint32_t)(total * src_rate) + 64;
    int16_t *src = malloc((size_t)src_cap * 2 * 2);
    uint32_t src_frames = 0;
    if (src) {
        static uint8_t raw[8192];
        int bitstream = 0;
        while (src_frames < src_cap) {
            long got = ov_read(&vf, (char *)raw, sizeof(raw), 0, 2, 1, &bitstream);
            if (got <= 0)
                break;
            long fr = got / (2L * (long)src_ch);
            int16_t *s = (int16_t *)raw;
            for (long i = 0; i < fr && src_frames < src_cap; i++) {
                if (src_ch == 1) {
                    src[(size_t)src_frames * 2 + 0] = s[0];
                    src[(size_t)src_frames * 2 + 1] = s[0];
                    s += 1;
                } else {
                    src[(size_t)src_frames * 2 + 0] = s[0];
                    src[(size_t)src_frames * 2 + 1] = s[1];
                    s += src_ch;
                }
                src_frames++;
            }
        }
    }
    ov_clear(&vf);
    if (!src || !src_frames) {
        free(src);
        free(pcm);
        return -1;
    }

    for (uint32_t i = 0; i < out_frames; i++) {
        double sp = (double)i * (double)src_rate / (double)OUT_RATE;
        uint32_t s0 = (uint32_t)sp;
        double fr = sp - s0;
        if (s0 >= src_frames - 1) {
            s0 = src_frames - 1;
            fr = 0.0;
        }
        for (int c = 0; c < 2; c++) {
            int a = src[(size_t)s0 * 2 + c];
            int b = src[(size_t)(s0 + (fr > 0 ? 1 : 0)) * 2 + c];
            pcm[(size_t)i * 2 + c] = (int16_t)(a + (b - a) * fr);
        }
    }
    free(src);

    *out_pcm = pcm;
    *out_frames_p = out_frames;
    return 0;
}

/* Installs already-decoded PCM into the cache. Mutates sfx_cache/sfx_voices,
 * so the caller MUST hold audio_mutex. Cheap (no I/O), unlike the decode
 * above. */
static sfx_entry_t *sfx_cache_install(int index, int16_t *pcm, uint32_t frames) {
    int slot = -1;
    for (int i = 0; i < SFX_CACHE_MAX; i++)
        if (!sfx_cache[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) {
        /* Evict least recently used slot; voices referencing it are
         * stopped first to avoid a dangling pointer. */
        uint32_t min_tick = 0xFFFFFFFF;
        int lru_slot = 0;
        for (int i = 0; i < SFX_CACHE_MAX; i++) {
            if (sfx_cache[i].last_used < min_tick) {
                min_tick = sfx_cache[i].last_used;
                lru_slot = i;
            }
        }
        slot = lru_slot;
        for (int v = 0; v < MAX_SFX_VOICES; v++)
            if (sfx_voices[v].active && sfx_voices[v].e == &sfx_cache[slot])
                sfx_voices[v].active = 0;
        free(sfx_cache[slot].pcm);
        sfx_cache[slot].used = 0;
    }
    sfx_cache[slot].used = 1;
    sfx_cache[slot].last_used = ++sfx_tick;
    sfx_cache[slot].index = index;
    sfx_cache[slot].pcm = pcm;
    sfx_cache[slot].frames = frames;
    return &sfx_cache[slot];
}


/* ---------------- Big (streamed) voices ---------------- */

/* Decode buffer per voice, in source frames. Must comfortably cover one
 * mixer tick's worth of source audio (OUT_FRAMES * step) with slack for
 * a source rate close to OUT_RATE; see the fill loop below. */
#define BIG_BUF_CAP (OUT_FRAMES + 64)

typedef struct big_stream {
    OggVorbis_File vf;
    long rate;
    int channels;
    struct big_stream *next;
} big_stream_t;

static big_stream_t *retired_streams;

static void wake_loader(void) {
    if (audio_load_sema >= 0) sceKernelSignalSema(audio_load_sema, 1);
}

typedef struct {
    int active;
    int paused;
    int index;
    int loop;
    float vol;
    big_stream_t *stream;
    int16_t buf[BIG_BUF_CAP * 2]; /* stereo, source rate, compacted each tick */
    int buf_frames;               /* valid frames held in buf[0..buf_frames) */
    double frac_pos;              /* fractional read position within buf */
    int eof;                      /* stream exhausted, non-looping: drain then stop */
    int pending_open;
    unsigned open_gen; /* Invalidates a worker result when stopped or reused. */
} big_voice_t;

static big_voice_t big_voices[MAX_BIG_VOICES];

/* Called under audio_mutex. Decoder destruction can close a file, so the
 * loader owns retirement as well as construction. Initialized Vorbis state
 * stays at a stable heap address for its complete lifetime. */
static void big_close(big_voice_t *v) {
    if (v->stream) {
        v->stream->next = retired_streams;
        retired_streams = v->stream;
        v->stream = NULL;
        wake_loader();
    }
    v->active = v->paused = v->buf_frames = v->eof = v->pending_open = 0;
    v->frac_pos = 0.0;
}

static big_stream_t *big_open(int index) {
    char path[512];
    snd_path(index, path, sizeof(path));
    if (!path[0]) return NULL;
    big_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { free(stream); return NULL; }
    if (ov_open(f, &stream->vf, NULL, 0) < 0) {
        fclose(f);
        free(stream);
        return NULL;
    }
    vorbis_info *vi = ov_info(&stream->vf, -1);
    stream->rate = vi && vi->rate > 0 ? vi->rate : 44100;
    stream->channels = vi && vi->channels > 0 ? vi->channels : 1;
    static unsigned open_logs;
    if (open_logs++ < 4)
        l_note("[AUDIO] stream opened: %s rate=%ld channels=%d", path, stream->rate, stream->channels);
    return stream;
}

/* Full SFX decode and stream opening run on the loader, never on the
 * output thread. Requests carry cancellation generations; only the mutex
 * protected handoff publishes worker-owned buffers/decoders. */
#define MAX_PENDING_SFX 8
#define MAX_PENDING_BIG 4

typedef struct {
    int index;
    float volume;
    float pitch;
    unsigned gen;
    int activate; /* 1 = audio_play() (decode + start a voice), 0 =
                   * audio_load() (decode-only preload, no voice) */
} pending_sfx_req_t;

typedef struct {
    int slot;   /* big_voices[] slot already reserved (pending_open=1) by the caller */
    int index;
    unsigned gen;
} pending_big_req_t;

static pending_sfx_req_t pending_sfx[MAX_PENDING_SFX];
static int pending_sfx_count = 0;
static pending_sfx_req_t inflight_sfx;
static int inflight_sfx_valid;
static pending_big_req_t pending_big[MAX_PENDING_BIG];
static int pending_big_count = 0;
static struct { unsigned gen; int paused; } sfx_control[GMV_SOUND_COUNT];

/* Called with audio_mutex held. Cheap (struct copy only, no I/O): coalesces
 * a repeated request for the same index instead of growing the queue. A
 * later audio_play() (activate=1) upgrades an already-queued audio_load()
 * preload (activate=0) for the same index; a later audio_load() never
 * downgrades one that is already set to play. */
static void enqueue_play_sfx_locked(int index, float volume, float pitch, int activate) {
    if (inflight_sfx_valid && inflight_sfx.index == index &&
        inflight_sfx.gen == sfx_control[index].gen) {
        if (activate) {
            inflight_sfx.volume = volume;
            inflight_sfx.pitch = pitch;
            inflight_sfx.activate = 1;
        }
        return;
    }
    for (int i = 0; i < pending_sfx_count; i++) {
        if (pending_sfx[i].index == index) {
            if (activate) {
                pending_sfx[i].volume = volume;
                pending_sfx[i].pitch = pitch;
                pending_sfx[i].activate = 1;
            }
            return;
        }
    }
    int slot = pending_sfx_count;
    if (slot == MAX_PENDING_SFX && activate) {
        for (int i = 0; i < pending_sfx_count; i++)
            if (!pending_sfx[i].activate) { slot = i; break; }
    }
    if (slot == MAX_PENDING_SFX) return;
    pending_sfx[slot] = (pending_sfx_req_t){ .index = index, .volume = volume,
        .pitch = pitch, .activate = activate, .gen = sfx_control[index].gen };
    if (slot == pending_sfx_count) pending_sfx_count++;
    wake_loader();
}

/* Called with audio_mutex held: removes slot's queued open request, if any
 * (used when a slot with a still-pending open gets forcibly reused). */
static void cancel_pending_big_locked(int slot) {
    for (int q = 0; q < pending_big_count; q++) {
        if (pending_big[q].slot == slot) {
            pending_big[q] = pending_big[--pending_big_count];
            return;
        }
    }
}

static void sfx_activate_locked(sfx_entry_t *entry, const pending_sfx_req_t *request) {
    int slot = 0;
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (!sfx_voices[i].active) { slot = i; break; }
    sfx_voices[slot] = (sfx_voice_t){ .active = 1,
        .paused = sfx_control[request->index].paused, .e = entry,
        .step_fix = (uint32_t)((double)request->pitch * 65536.0), .vol = request->volume };
}

static int audio_load_thread(SceSize argc, void *argv) {
    (void)argc; (void)argv;
    unsigned slow_logs = 0;
    while (audio_running) {
        sceKernelWaitSema(audio_load_sema, 1, NULL);
        while (audio_running) {
            big_stream_t *retired = NULL;
            pending_sfx_req_t sfx_request = {0};
            pending_big_req_t big_request = {0};
            int kind = 0;
            audio_lock();
            if (retired_streams) {
                retired = retired_streams;
                retired_streams = retired->next;
                kind = 1;
            } else if (pending_big_count) {
                big_request = pending_big[--pending_big_count];
                kind = 2;
            } else if (pending_sfx_count) {
                int slot = 0;
                for (int i = 0; i < pending_sfx_count; i++)
                    if (pending_sfx[i].activate) { slot = i; break; }
                sfx_request = pending_sfx[slot];
                inflight_sfx = sfx_request;
                inflight_sfx_valid = 1;
                pending_sfx[slot] = pending_sfx[--pending_sfx_count];
                kind = 3;
            }
            sceKernelUnlockMutex(audio_mutex, 1);
            if (!kind) break;
            if (retired) { ov_clear(&retired->vf); free(retired); continue; }
            SceUInt64 started = sceKernelGetProcessTimeWide();
            if (kind == 2) {
                big_stream_t *stream = big_open(big_request.index);
                audio_lock();
                big_voice_t *voice = &big_voices[big_request.slot];
                if (voice->pending_open && voice->open_gen == big_request.gen &&
                    voice->index == big_request.index) {
                    voice->pending_open = 0;
                    voice->stream = stream;
                    voice->active = stream != NULL;
                    voice->buf_frames = voice->eof = 0;
                    voice->frac_pos = 0.0;
                    stream = NULL;
                }
                sceKernelUnlockMutex(audio_mutex, 1);
                if (stream) { ov_clear(&stream->vf); free(stream); }
            } else {
                int16_t *pcm = NULL;
                uint32_t frames = 0;
                audio_lock();
                sfx_request = inflight_sfx;
                int current = sfx_control[sfx_request.index].gen == sfx_request.gen;
                sfx_entry_t *entry = current ? sfx_find(sfx_request.index) : NULL;
                if (entry && sfx_request.activate) sfx_activate_locked(entry, &sfx_request);
                if (entry || !current) inflight_sfx_valid = 0;
                sceKernelUnlockMutex(audio_mutex, 1);
                if (current && !entry && sfx_decode_raw(sfx_request.index, &pcm, &frames) == 0) {
                    audio_lock();
                    sfx_request = inflight_sfx;
                    if (sfx_control[sfx_request.index].gen == sfx_request.gen) {
                        entry = sfx_find(sfx_request.index);
                        if (!entry) {
                            entry = sfx_cache_install(sfx_request.index, pcm, frames);
                            pcm = NULL;
                        }
                        if (entry && sfx_request.activate) sfx_activate_locked(entry, &sfx_request);
                    }
                    inflight_sfx_valid = 0;
                    sceKernelUnlockMutex(audio_mutex, 1);
                    free(pcm);
                }
                audio_lock();
                inflight_sfx_valid = 0;
                sceKernelUnlockMutex(audio_mutex, 1);
            }
            SceUInt64 elapsed = sceKernelGetProcessTimeWide() - started;
            if (elapsed >= 100000 && slow_logs++ < 16)
                l_note("[AUDIO] loader kind=%d index=%d took=%llu ms", kind,
                       kind == 2 ? big_request.index : sfx_request.index, elapsed / 1000);
        }
    }
    return 0;
}

/* ---------------- Mixer thread ---------------- */

static int16_t mix_bufs[2][OUT_FRAMES * 2] __attribute__((aligned(64)));

/* Soft-knee limiter: passes normal levels through untouched and only
 * compresses (never hard-clips) once |s| goes past KNEE, so overlapping
 * voices saturate smoothly instead of producing the harsh square-wave
 * distortion a hard clamp gives. */
static int16_t soft_clip16(int32_t s) {
    const int32_t KNEE = 24000;
    const int32_t HEAD = 32767 - KNEE;
    int32_t a = s < 0 ? -s : s;
    if (a <= KNEE)
        return (int16_t)s;
    int32_t over = a - KNEE;
    int32_t comp = KNEE + (int32_t)((float)HEAD * (float)over / (float)(over + HEAD));
    return (int16_t)(s < 0 ? -comp : comp);
}

static int audio_mix_thread(SceSize argc, void *argv) {
    (void)argc;
    (void)argv;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
        l_note("[AUDIO] mixer priority=%d affinity=0x%x", info.currentPriority,
               info.currentCpuAffinityMask);
    SceUInt64 report_at = sceKernelGetProcessTimeWide(), last_submit = 0;
    SceUInt64 work_sum = 0, work_max = 0, mix_max = 0, lock_max = 0;
    SceUInt64 output_sum = 0, gap_max = 0;
    unsigned ticks = 0, late = 0, output_errors = 0, reports = 0;
    const SceUInt64 budget_us = (SceUInt64)OUT_FRAMES * 1000000 / OUT_RATE;
    unsigned buffer_index = 0;
    while (audio_running) {
        int16_t *mix_buf = mix_bufs[buffer_index];
        SceUInt64 tick_start = sceKernelGetProcessTimeWide();
        int32_t acc[OUT_FRAMES * 2];
        memset(acc, 0, sizeof(acc));

        SceUInt64 lock_started = sceKernelGetProcessTimeWide();
        audio_lock();
        SceUInt64 lock_acquired = sceKernelGetProcessTimeWide();

        if (!audio_blocked) {
            for (int i = 0; i < MAX_SFX_VOICES; i++) {
                sfx_voice_t *v = &sfx_voices[i];
                if (!v->active || v->paused || !v->e || !v->e->pcm)
                    continue;
                float g = v->vol * cat_gain(v->e->index);
                if (g > 1.5f) g = 1.5f;
                for (int n = 0; n < OUT_FRAMES; n++) {
                    uint32_t p = v->pos_fix >> 16;
                    if (p >= v->e->frames) {
                        v->active = 0;
                        break;
                    }
                    acc[n * 2 + 0] += (int32_t)(v->e->pcm[p * 2 + 0] * g);
                    acc[n * 2 + 1] += (int32_t)(v->e->pcm[p * 2 + 1] * g);
                    v->pos_fix += v->step_fix;
                }
            }

            for (int i = 0; i < MAX_BIG_VOICES; i++) {
                big_voice_t *v = &big_voices[i];
                if (!v->active || v->paused || !v->stream)
                    continue;
                float g = v->vol * cat_gain(v->index);
                if (g > 1.5f) g = 1.5f;
                double step = (double)v->stream->rate / (double)OUT_RATE;

                /* Top off the per-voice decode buffer, appending after
                 * whatever is already held. Earlier this window was
                 * rebuilt from scratch every tick and any source frames
                 * decoded past the mixed position were thrown away --
                 * ~7-8 frames per 2048-sample buffer, a periodic skip at
                 * the ~23 Hz buffer rate that read back as a harsh buzz
                 * riding on top of the music. Carrying the tail forward
                 * (compacted below) makes the position sample-accurate
                 * across ticks: nothing decoded is ever discarded. */
                uint32_t target = (uint32_t)(v->frac_pos + (double)OUT_FRAMES * step) + 4;
                if (target > BIG_BUF_CAP)
                    target = BIG_BUF_CAP;
                int bitstream = 0;
                int loop_retries = 0;
                static uint8_t braw[8192];
                while (!v->eof && (uint32_t)v->buf_frames < target) {
                    long want = (long)(target - v->buf_frames) * 2L * (long)v->stream->channels;
                    if (want > (long)sizeof(braw))
                        want = sizeof(braw);
                    long got = ov_read(&v->stream->vf, (char *)braw, (int)want,
                                       0, 2, 1, &bitstream);
                    if (got <= 0) {
                        if (v->loop && ++loop_retries <= 4) {
                            ov_time_seek(&v->stream->vf, 0.0);
                            continue;
                        }
                        v->eof = 1;
                        break;
                    }
                    long fr = got / (2L * (long)v->stream->channels);
                    int16_t *s = (int16_t *)braw;
                    for (long f = 0; f < fr && (uint32_t)v->buf_frames < BIG_BUF_CAP; f++) {
                        if (v->stream->channels == 1) {
                            v->buf[(size_t)v->buf_frames * 2 + 0] = s[0];
                            v->buf[(size_t)v->buf_frames * 2 + 1] = s[0];
                            s += 1;
                        } else {
                            v->buf[(size_t)v->buf_frames * 2 + 0] = s[0];
                            v->buf[(size_t)v->buf_frames * 2 + 1] = s[1];
                            s += v->stream->channels;
                        }
                        v->buf_frames++;
                    }
                }

                if (v->buf_frames < 2) {
                    if (v->eof && !v->loop)
                        big_close(v);
                    continue;
                }

                double sp = v->frac_pos;
                for (int n = 0; n < OUT_FRAMES; n++) {
                    uint32_t s0 = (uint32_t)sp;
                    if (s0 + 1 >= (uint32_t)v->buf_frames)
                        break; /* caught up with the decoder; resume next tick */
                    double fr = sp - s0;
                    for (int c = 0; c < 2; c++) {
                        int a = v->buf[(size_t)s0 * 2 + c];
                        int b = v->buf[(size_t)(s0 + 1) * 2 + c];
                        int s = (int)(a + (b - a) * fr);
                        acc[n * 2 + c] += (int32_t)(s * g);
                    }
                    sp += step;
                }

                /* Compact: drop whole frames actually consumed, keep the
                 * fractional remainder plus any decoded-but-unconsumed
                 * tail for next tick. */
                int consumed = (int)sp;
                if (consumed > v->buf_frames)
                    consumed = v->buf_frames;
                if (consumed > 0) {
                    memmove(v->buf, v->buf + (size_t)consumed * 2,
                            (size_t)(v->buf_frames - consumed) * 2 * sizeof(int16_t));
                    v->buf_frames -= consumed;
                    v->frac_pos = sp - consumed;
                } else {
                    v->frac_pos = sp;
                }

                if (v->buf_frames == 0 && v->eof && !v->loop)
                    big_close(v);
            }
        }

        SceUInt64 mix_finished = sceKernelGetProcessTimeWide();
        sceKernelUnlockMutex(audio_mutex, 1);

        for (int n = 0; n < OUT_FRAMES * 2; n++)
            mix_buf[n] = soft_clip16(acc[n]);
        SceUInt64 submit_at = sceKernelGetProcessTimeWide();
        int result = audio_port >= 0 ? sceAudioOutOutput(audio_port, mix_buf) : -1;
        SceUInt64 done_at = sceKernelGetProcessTimeWide();
        buffer_index ^= 1;
        SceUInt64 work = submit_at - tick_start;
        SceUInt64 mix = mix_finished - lock_acquired;
        SceUInt64 lock_wait = lock_acquired - lock_started;
        if (lock_wait > lock_max) lock_max = lock_wait;
        work_sum += work;
        output_sum += done_at - submit_at;
        if (work > work_max) work_max = work;
        if (mix > mix_max) mix_max = mix;
        if (last_submit && submit_at - last_submit > gap_max)
            gap_max = submit_at - last_submit;
        last_submit = submit_at;
        late += work > budget_us;
        output_errors += result < 0;
        ticks++;
        if (done_at - report_at >= 5000000) {
            if (reports < 4 || late || output_errors || gap_max > budget_us + 2000)
                l_note("[AUDIO] ticks=%u work_avg=%llu work_max=%llu "
                       "mix_max=%llu lock_max=%llu output_avg=%llu gap_max=%llu late=%u errors=%u (us)",
                       ticks, work_sum / ticks, work_max, mix_max, lock_max,
                       output_sum / ticks, gap_max, late, output_errors);
            reports++;
            report_at = done_at;
            work_sum = work_max = mix_max = lock_max = output_sum = gap_max = 0;
            ticks = late = output_errors = 0;
        }
    }
    return 0;
}

void audio_init(void) {
    if (audio_port >= 0) return;
    audio_api_thread = sceKernelGetThreadId();
    int loader_started = 0, mixer_started = 0;
    audio_mutex = sceKernelCreateMutex("gmv_audio", 0, 0, NULL);
    if (audio_mutex < 0) goto failed;
    audio_load_sema = sceKernelCreateSema("gmv_audio_load", 0, 0, 1, NULL);
    if (audio_load_sema < 0) goto failed;
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_FRAMES,
                                     OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) goto failed;
    audio_loader_thread = sceKernelCreateThread("gmv_audio_load", audio_load_thread,
                                191, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (audio_loader_thread < 0) goto failed;
    audio_thread = sceKernelCreateThread("gmv_audio_mix", audio_mix_thread,
                                64, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (audio_thread < 0) goto failed;
    audio_running = 1;
    if (sceKernelStartThread(audio_loader_thread, 0, NULL) < 0) goto failed;
    loader_started = 1;
    if (sceKernelStartThread(audio_thread, 0, NULL) < 0) goto failed;
    mixer_started = 1;
    l_note("[AUDIO] backend up: %d sounds, port %d @%dHz, separate loader", GMV_SOUND_COUNT, audio_port, OUT_RATE);
    return;
failed:
    l_error("audio: initialization failed (mutex=%d sema=%d port=%d loader=%d mixer=%d)",
            audio_mutex, audio_load_sema, audio_port, audio_loader_thread, audio_thread);
    audio_running = 0;
    wake_loader();
    if (loader_started) sceKernelWaitThreadEnd(audio_loader_thread, NULL, NULL);
    if (mixer_started) sceKernelWaitThreadEnd(audio_thread, NULL, NULL);
    if (audio_loader_thread >= 0) sceKernelDeleteThread(audio_loader_thread);
    if (audio_thread >= 0) sceKernelDeleteThread(audio_thread);
    if (audio_port >= 0) sceAudioOutReleasePort(audio_port);
    if (audio_load_sema >= 0) sceKernelDeleteSema(audio_load_sema);
    if (audio_mutex >= 0) sceKernelDeleteMutex(audio_mutex);
    audio_loader_thread = audio_thread = audio_port = audio_load_sema = audio_mutex = -1;
}

/* ---------------- Public API ---------------- */

void audio_load(int index) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    audio_lock();
    sfx_entry_t *hit = sfx_find(index);
    if (!hit)
        enqueue_play_sfx_locked(index, 0.0f, 1.0f, 0 /* decode-only, no voice */);
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_play(int index, float volume, float pitch) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    volume = sane_vol(volume);
    pitch = sane_pitch(pitch);
    static int logged = 0;
    if (!logged) {
        logged = 1;
        l_note("[AUDIO] first play: idx=%d vol=%.2f pitch=%.2f", index, volume, pitch);
    }
    audio_lock();
    sfx_control[index].paused = 0;
    /* Fast path: already decoded and cached -- cheap, no I/O, stays
     * synchronous exactly like before. */
    sfx_entry_t *e = sfx_find(index);
    if (e) {
        /* Steal the first free (or oldest = slot 0) voice. */
        int slot = -1;
        for (int i = 0; i < MAX_SFX_VOICES; i++)
            if (!sfx_voices[i].active) {
                slot = i;
                break;
            }
        if (slot < 0)
            slot = 0;
        sfx_voices[slot].active = 1;
        sfx_voices[slot].paused = 0;
        sfx_voices[slot].e = e;
        sfx_voices[slot].pos_fix = 0;
        sfx_voices[slot].step_fix = (uint32_t)((double)pitch * 65536.0);
        sfx_voices[slot].vol = volume;
        sceKernelUnlockMutex(audio_mutex, 1);
        return;
    }
    /* The loader publishes PCM when ready; the caller and output thread
     * never wait for a full sound decode. */
    enqueue_play_sfx_locked(index, volume, pitch, 1 /* start a voice once decoded */);
    sceKernelUnlockMutex(audio_mutex, 1);
}

static void sfx_set_paused(int index, int paused) {
    audio_lock();
    if (index >= 0 && index < GMV_SOUND_COUNT) sfx_control[index].paused = paused;
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].paused = paused;
    sceKernelUnlockMutex(audio_mutex, 1);
}

static void sfx_stop_locked(int index) {
    if (index >= 0 && index < GMV_SOUND_COUNT) sfx_control[index].gen++;
    for (int i = pending_sfx_count - 1; i >= 0; i--)
        if (pending_sfx[i].index == index) pending_sfx[i] = pending_sfx[--pending_sfx_count];
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].active = 0;
}

static void sfx_stop(int index) {
    audio_lock();
    sfx_stop_locked(index);
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_sfx(int index, int instance) {
    (void)instance;
    sfx_set_paused(index, 1);
}

void audio_resume_sfx(int index, int instance) {
    (void)instance;
    sfx_set_paused(index, 0);
}

void audio_stop_sfx(int index, int instance) {
    (void)instance;
    sfx_stop(index);
}

void audio_unload(int index) {
    audio_lock();
    sfx_stop_locked(index);
    sfx_entry_t *e = sfx_find(index);
    if (e) {
        free(e->pcm);
        e->pcm = NULL;
        e->used = 0;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_volume_sfx(int index, int instance, float vol) {
    (void)instance;
    vol = sane_vol(vol);
    audio_lock();
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].vol = vol;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_pitch(int index, int instance, float pitch) {
    (void)instance;
    pitch = sane_pitch(pitch);
    audio_lock();
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        if (sfx_voices[i].active && sfx_voices[i].e &&
            sfx_voices[i].e->index == index)
            sfx_voices[i].step_fix = (uint32_t)((double)pitch * 65536.0);
    sceKernelUnlockMutex(audio_mutex, 1);
}

int audio_is_loaded(int index) {
    if (index < 0 || index >= GMV_SOUND_COUNT)
        return 0;
    audio_lock();
    int ok = sfx_find(index) ? 1 : 0;
    sceKernelUnlockMutex(audio_mutex, 1);
    if (!ok) {
        ok = snd_exists(index);
    }
    return ok;
}

void audio_load_big(int index) {
    /* Streaming needs no preload; validated lazily on play. */
    (void)index;
}

void audio_play_big(int index, float volume, int loop) {
    if (audio_port < 0 || index < 0 || index >= GMV_SOUND_COUNT)
        return;
    volume = sane_vol(volume);
    if (!loop && snd_is_effect(index)) {
        static unsigned routed_effects;
        if (routed_effects++ < 8)
            l_note("[AUDIO] one-shot big sound uses decoded cache: idx=%d file=%s",
                   index, gmv_sound_files[index]);
        audio_play(index, volume, 1.0f);
        return;
    }
    static int logged_big = 0;
    if (!logged_big) {
        logged_big = 1;
        l_note("[AUDIO] first play_big: idx=%d vol=%.2f loop=%d", index, volume, loop);
    }
    audio_lock();
    /* Same index replayed (the per-frame radio re-issue while already
     * playing) is a no-op: this single dedup is what settles the
     * playRadio/stopRadio churn once state reporting is correct. */
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && big_voices[i].index == index) {
            big_voices[i].vol = volume;
            big_voices[i].loop = loop ? 1 : 0;
            big_voices[i].paused = 0;
            sceKernelUnlockMutex(audio_mutex, 1);
            return;
        }

    int slot = -1;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if (!big_voices[i].active && !big_voices[i].pending_open) {
            slot = i;
            break;
        }
    if (slot < 0) {
        /* Pending slots own an immutable loader request; only evict a
         * published stream, retiring its decoder on the loader. */
        for (int i = 0; i < MAX_BIG_VOICES; i++)
            if (!big_voices[i].pending_open) {
                slot = i;
                break;
            }
        if (slot < 0) {
            sceKernelUnlockMutex(audio_mutex, 1);
            return;
        }
        cancel_pending_big_locked(slot);
        big_close(&big_voices[slot]);
    }
    big_voice_t *v = &big_voices[slot];
    if (v->stream)
        big_close(v);
    v->pending_open = 1;
    v->vol = volume;
    v->loop = loop != 0;
    v->paused = 0;
    v->index = index;
    v->open_gen++; /* new reservation epoch -- voids any in-flight open */
    if (pending_big_count < MAX_PENDING_BIG) {
        pending_big[pending_big_count].slot = slot;
        pending_big[pending_big_count].index = index;
        pending_big[pending_big_count].gen = v->open_gen;
        pending_big_count++;
        wake_loader();
    } else {
        /* Can't happen in practice (MAX_PENDING_BIG == MAX_BIG_VOICES, at
         * most one request per slot) -- don't leave the slot stuck
         * pending forever if it ever did. */
        v->pending_open = 0;
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_big(int index) {
    audio_lock();
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && big_voices[i].index == index)
            big_voices[i].paused = 1;
    sceKernelUnlockMutex(audio_mutex, 1);
    if (snd_is_effect(index))
        audio_pause_sfx(index, 0);
}

void audio_resume_big(int index) {
    /* Unpause the matching stream; if it was fully stopped, restart it. */
    if (snd_is_effect(index)) {
        audio_resume_sfx(index, 0);
        if (audio_is_media_playing(index))
            return;
    }
    int restarted = 0;
    audio_lock();
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && big_voices[i].index == index) {
            big_voices[i].paused = 0;
            restarted = 1;
        }
    sceKernelUnlockMutex(audio_mutex, 1);
    if (!restarted)
        audio_play_big(index, 1.0f, 0);
}

void audio_stop_big(int index) {
    /* Fase 59: a stop also cancels a still-pending open (and voids an
     * in-flight ov_open via the generation bump) -- previously a stop
     * arriving during the deferred-open window was a silent no-op and
     * the stream started anyway right after, which is exactly the
     * stopRadio/playRadio churn the engine falls into when it polls
     * state per frame (log 058 while driving). */
    audio_lock();
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        big_voice_t *v = &big_voices[i];
        if (index >= 0 && v->index != index)
            continue;
        if (v->pending_open) {
            cancel_pending_big_locked(i);
            v->pending_open = 0;
            v->open_gen++;
        }
        if (v->active)
            big_close(v);
    }
    if (index < 0) {
        for (int i = 0; i < GMV_SOUND_COUNT; i++)
            sfx_stop_locked(i);
    } else if (snd_is_effect(index)) {
        sfx_stop_locked(index);
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_unload_big(int index) {
    audio_stop_big(index);
}

void audio_set_volume_big(int index, float vol) {
    vol = sane_vol(vol);
    audio_lock();
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && big_voices[i].index == index)
            big_voices[i].vol = vol;
    sceKernelUnlockMutex(audio_mutex, 1);
    if (snd_is_effect(index))
        audio_set_volume_sfx(index, 0, vol);
}

int audio_is_loaded_big(int index) {
    return snd_exists(index);
}

int audio_is_media_playing(int index) {
    /* Fase 59: a slot with a still-pending open counts as playing.
     * SoundManager::update() polls this every frame and re-issues
     * playRadio/playSound on "not playing" -- reporting 0 during the
     * deferred-open window (~one mixer tick) is what kept the engine
     * re-firing stop+play pairs back-to-back (log 058: dozens of
     * consecutive stopRadio/playRadio lines while driving). A queued
     * one-shot decode has the same lifetime: it will publish a voice or
     * be cancelled by stop, so count it as playing while it is current. */
    int playing = 0;
    audio_lock();
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        if ((big_voices[i].active || big_voices[i].pending_open) && !big_voices[i].paused &&
            (index < 0 || big_voices[i].index == index)) {
            playing = 1;
            break;
        }
    if (!playing) {
        for (int i = 0; i < MAX_SFX_VOICES; i++)
            if (sfx_voices[i].active && !sfx_voices[i].paused &&
                (index < 0 || (sfx_voices[i].e && sfx_voices[i].e->index == index))) {
                playing = 1;
                break;
            }
    }
    if (!playing && inflight_sfx_valid && inflight_sfx.activate &&
        sfx_control[inflight_sfx.index].gen == inflight_sfx.gen &&
        !sfx_control[inflight_sfx.index].paused &&
        (index < 0 || inflight_sfx.index == index))
        playing = 1;
    if (!playing) for (int i = 0; i < pending_sfx_count; i++)
        if (pending_sfx[i].activate &&
            sfx_control[pending_sfx[i].index].gen == pending_sfx[i].gen &&
            !sfx_control[pending_sfx[i].index].paused &&
            (index < 0 || pending_sfx[i].index == index)) {
            playing = 1;
            break;
        }
    sceKernelUnlockMutex(audio_mutex, 1);
    return playing;
}

void audio_stop_all(void) {
    audio_lock();
    for (int i = 0; i < GMV_SOUND_COUNT; i++) sfx_control[i].gen++;
    pending_sfx_count = 0;
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].active = 0;
    for (int i = 0; i < MAX_BIG_VOICES; i++) {
        /* Fase 59: same pending-cancel as audio_stop_big -- a queued or
         * in-flight open must not start playing after a stop-all. */
        if (big_voices[i].pending_open) {
            cancel_pending_big_locked(i);
            big_voices[i].pending_open = 0;
            big_voices[i].open_gen++;
        }
        if (big_voices[i].active)
            big_close(&big_voices[i]);
    }
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_pause_all(void) {
    audio_lock();
    for (int i = 0; i < GMV_SOUND_COUNT; i++) sfx_control[i].paused = 1;
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].paused = 1;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        big_voices[i].paused = 1;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_resume_all(void) {
    audio_lock();
    for (int i = 0; i < GMV_SOUND_COUNT; i++) sfx_control[i].paused = 0;
    for (int i = 0; i < MAX_SFX_VOICES; i++)
        sfx_voices[i].paused = 0;
    for (int i = 0; i < MAX_BIG_VOICES; i++)
        big_voices[i].paused = 0;
    sceKernelUnlockMutex(audio_mutex, 1);
}

void audio_set_gains(float music, float sfx, float vfx) {
    if (music >= 0.0f && music <= 8.0f && music != gain_music) {
        gain_music = music;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
    if (sfx >= 0.0f && sfx <= 8.0f && sfx != gain_sfx) {
        gain_sfx = sfx;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
    if (vfx >= 0.0f && vfx <= 8.0f && vfx != gain_vfx) {
        gain_vfx = vfx;
        l_note("[AUDIO] gains: music=%.2f sfx=%.2f vfx=%.2f", gain_music, gain_sfx, gain_vfx);
    }
}

void audio_block(int blocked) {
    audio_blocked = blocked ? 1 : 0;
}
