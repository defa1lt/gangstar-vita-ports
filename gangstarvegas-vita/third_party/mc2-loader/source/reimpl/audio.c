#include "reimpl/audio.h"

#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#include "reimpl/io.h"

extern void port_trace(const char *format, ...);

/*
 * Sandstorm 2 delegates all Ogg playback to GLMediaPlayer/SoundPool Java
 * callbacks.  There is no Java VM in a soloader port, so provide the small
 * part of that service the game actually needs: Ogg decoding, 48 kHz stereo
 * resampling and a software mixer feeding one Vita MAIN audio port.
 */
enum {
    MC2_AUDIO_ASSETS = 2048,
    MC2_AUDIO_INITIAL_VOICES = 64,
    MC2_AUDIO_FRAMES = 2048,
    MC2_AUDIO_RATE = 48000,
    MC2_LIMITER_CHUNK_FRAMES = 32,
    MC2_LIMITER_TARGET_PEAK = 32000,
    MC2_LIMITER_RELEASE_SHIFT = 10,
    MC2_SOUND_DECODE_WORKERS = 2,
    MC2_AUDIO_CACHE_LIMIT = 56 * 1024 * 1024,
    MC2_AUDIO_ASSET_LIMIT = 48 * 1024 * 1024,
};

typedef struct {
    char path[256];
    int valid;
    int16_t *pcm;
    uint32_t frames;
    uint32_t bytes;
    uint32_t last_used;
    uint32_t generation;
    int volume;
    int decode_requested;
    int decoding;
    int decode_failed;
    int pending_music;
    int pending_sound;
    int pending_loop;
    int pending_volume;
} Mc2AudioAsset;

typedef struct {
    Mc2AudioAsset *asset;
    uint32_t position;
    int loops;
    int volume;
    int paused;
    int active;
} Mc2AudioVoice;

typedef struct {
    int fd;
} Mc2OggSource;

static Mc2AudioAsset music_assets[MC2_AUDIO_ASSETS];
static Mc2AudioAsset sound_assets[MC2_AUDIO_ASSETS];
static Mc2AudioVoice music_voice;
static Mc2AudioVoice *sound_voices;
static unsigned sound_voice_capacity;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static int audio_thread_started;
static pthread_t music_decode_thread;
static pthread_t sound_decode_threads[MC2_SOUND_DECODE_WORKERS];
static int music_decode_thread_started;
static int sound_decode_thread_started;
static pthread_cond_t music_decode_condition = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sound_decode_condition = PTHREAD_COND_INITIALIZER;
static uint32_t audio_clock;
static uint32_t audio_cache_bytes;
static int audio_paused;
static unsigned audio_stats_max_voices;
static uint64_t audio_stats_max_peak;
static unsigned audio_stats_voice_growth_failures;
static unsigned audio_stats_min_gain = 32768;
static unsigned audio_stats_limited_chunks;
static unsigned audio_stats_late_buffers;
static int16_t audio_output[2][MC2_AUDIO_FRAMES * 2]
    __attribute__((aligned(64)));

static int valid_id(int id) {
    return id >= 0 && id < MC2_AUDIO_ASSETS;
}

static int clamp_volume(float volume) {
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;
    return (int)(volume * 32768.0f + 0.5f);
}

static void make_audio_path(const char *name, char output[256]) {
    if (!name)
        name = "";
    /* Purple keeps the legacy Android/early-port path in its Java audio
     * registry (ux0:data/mc2//data/audio/...).  The decoder below uses
     * sceIoOpen directly, so that path never passes through fopen_soloader's
     * general data redirection.  Preserve paths that already point at this
     * port, but remap every legacy/Android audio name to the installed data
     * directory by basename. */
    if (!strncmp(name, "_packRes/", 9) ||
        !strncmp(name, "_packEtc/", 9) ||
        !strncmp(name, DATA_PATH, strlen(DATA_PATH)) ||
        !strncmp(name, "app0:", 5)) {
        snprintf(output, 256, "%s", name);
        return;
    }

    const char *base = name;
    for (const char *cursor = name; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\')
            base = cursor + 1;
    }
    snprintf(output, 256, DATA_PATH "GloftBPHP/data/audio/%s", base);
}

static uint16_t read_u16_le(const unsigned char *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const unsigned char *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int resample_pcm_48k(const int16_t *input, uint32_t source_frames,
                            int source_rate, int source_channels,
                            int16_t **output_pcm, uint32_t *output_frames,
                            uint32_t *output_bytes) {
    if (!input || !source_frames || source_rate <= 0 ||
        (source_channels != 1 && source_channels != 2)) {
        return 0;
    }

    uint64_t converted_frames64 =
        ((uint64_t)source_frames * MC2_AUDIO_RATE + source_rate - 1) /
        (uint32_t)source_rate;
    uint64_t converted_bytes64 = converted_frames64 * 2 * sizeof(int16_t);
    if (!converted_frames64 || converted_frames64 > UINT32_MAX ||
        converted_bytes64 > MC2_AUDIO_ASSET_LIMIT) {
        return 0;
    }

    uint32_t converted_frames = (uint32_t)converted_frames64;
    uint32_t converted_bytes = (uint32_t)converted_bytes64;
    int16_t *converted = (int16_t *)malloc(converted_bytes);
    if (!converted) return 0;

    for (uint32_t frame = 0; frame < converted_frames; ++frame) {
        uint64_t scaled = (uint64_t)frame * (uint32_t)source_rate;
        uint32_t first = (uint32_t)(scaled / MC2_AUDIO_RATE);
        uint32_t fraction = (uint32_t)(scaled % MC2_AUDIO_RATE);
        if (first >= source_frames) first = source_frames - 1;
        uint32_t second = first + 1 < source_frames ? first + 1 : first;
        for (unsigned channel = 0; channel < 2; ++channel) {
            unsigned source_channel = source_channels == 1 ? 0 : channel;
            int32_t a = input[(size_t)first * source_channels + source_channel];
            int32_t b = input[(size_t)second * source_channels + source_channel];
            converted[(size_t)frame * 2 + channel] = (int16_t)(
                (a * (int32_t)(MC2_AUDIO_RATE - fraction) +
                 b * (int32_t)fraction) / MC2_AUDIO_RATE);
        }
    }

    *output_pcm = converted;
    *output_frames = converted_frames;
    *output_bytes = converted_bytes;
    return 1;
}

static size_t ogg_read(void *ptr, size_t size, size_t count,
                       void *datasource) {
    Mc2OggSource *source = (Mc2OggSource *)datasource;
    if (!size || !count)
        return 0;
    size_t bytes = size * count;
    int read = sceIoRead(source->fd, ptr, bytes);
    return read > 0 ? (size_t)read / size : 0;
}

static int ogg_seek(void *datasource, ogg_int64_t offset, int whence) {
    Mc2OggSource *source = (Mc2OggSource *)datasource;
    int vita_whence = whence == SEEK_SET ? SCE_SEEK_SET :
                       whence == SEEK_CUR ? SCE_SEEK_CUR : SCE_SEEK_END;
    return sceIoLseek(source->fd, offset, vita_whence) < 0 ? -1 : 0;
}

static int ogg_close(void *datasource) {
    Mc2OggSource *source = (Mc2OggSource *)datasource;
    int result = sceIoClose(source->fd);
    source->fd = -1;
    return result < 0 ? -1 : 0;
}

static long ogg_tell(void *datasource) {
    Mc2OggSource *source = (Mc2OggSource *)datasource;
    SceOff position = sceIoLseek(source->fd, 0, SCE_SEEK_CUR);
    return position < 0 ? -1 : (long)position;
}

static int decode_ogg_48k(const char *path, int16_t **output_pcm,
                          uint32_t *output_frames, uint32_t *output_bytes) {
    static const ov_callbacks callbacks = {
        .read_func = ogg_read,
        .seek_func = ogg_seek,
        .close_func = ogg_close,
        .tell_func = ogg_tell,
    };
    Mc2OggSource source = { .fd = sceIoOpen(path, SCE_O_RDONLY, 0) };
    if (source.fd < 0) {
        port_trace("AUDIO: open failed path=%s result=0x%x", path, source.fd);
        return 0;
    }

    OggVorbis_File vorbis;
    int open_result = ov_open_callbacks(&source, &vorbis, NULL, 0, callbacks);
    if (open_result < 0) {
        port_trace("AUDIO: ov_open failed path=%s result=%d", path,
                   open_result);
        sceIoClose(source.fd);
        return 0;
    }

    vorbis_info *info = ov_info(&vorbis, -1);
    if (!info || info->rate <= 0 || info->channels <= 0) {
        port_trace("AUDIO: invalid Vorbis stream path=%s", path);
        ov_clear(&vorbis);
        return 0;
    }
    const int source_rate = (int)info->rate;
    const int source_channels = info->channels;

    ogg_int64_t reported_frames = ov_pcm_total(&vorbis, -1);
    size_t capacity = reported_frames > 0
        ? (size_t)reported_frames * (size_t)source_channels * sizeof(int16_t)
        : 256 * 1024;
    if (capacity < 16 * 1024)
        capacity = 16 * 1024;
    if (capacity > MC2_AUDIO_ASSET_LIMIT) {
        port_trace("AUDIO: stream too large path=%s decoded=%u", path,
                   (unsigned)capacity);
        ov_clear(&vorbis);
        return 0;
    }

    uint8_t *source_pcm = (uint8_t *)malloc(capacity);
    if (!source_pcm) {
        ov_clear(&vorbis);
        return 0;
    }

    size_t used = 0;
    int section = 0;
    unsigned holes = 0;
    for (;;) {
        if (capacity - used < 16 * 1024) {
            size_t next = capacity * 2;
            if (next > MC2_AUDIO_ASSET_LIMIT)
                next = MC2_AUDIO_ASSET_LIMIT;
            if (next <= capacity)
                break;
            void *grown = realloc(source_pcm, next);
            if (!grown)
                break;
            source_pcm = (uint8_t *)grown;
            capacity = next;
        }
        long result = ov_read(&vorbis, (char *)source_pcm + used,
                              (int)(capacity - used), 0, 2, 1, &section);
        if (result > 0) {
            used += (size_t)result;
        } else if (result == 0) {
            break;
        } else if (++holes > 32) {
            port_trace("AUDIO: Vorbis decode error path=%s result=%ld", path,
                       result);
            break;
        }
    }
    ov_clear(&vorbis);

    uint32_t source_frames =
        (uint32_t)(used / ((size_t)source_channels * sizeof(int16_t)));
    if (!source_frames) {
        free(source_pcm);
        return 0;
    }

    uint64_t converted_frames64 =
        ((uint64_t)source_frames * MC2_AUDIO_RATE + source_rate - 1) /
        (uint32_t)source_rate;
    uint64_t converted_bytes64 = converted_frames64 * 2 * sizeof(int16_t);
    if (!converted_frames64 || converted_frames64 > UINT32_MAX ||
        converted_bytes64 > MC2_AUDIO_ASSET_LIMIT) {
        port_trace("AUDIO: converted stream too large path=%s bytes=%u", path,
                   (unsigned)converted_bytes64);
        free(source_pcm);
        return 0;
    }

    uint32_t converted_frames = (uint32_t)converted_frames64;
    uint32_t converted_bytes = (uint32_t)converted_bytes64;
    int16_t *converted = (int16_t *)malloc(converted_bytes);
    if (!converted) {
        free(source_pcm);
        return 0;
    }

    const int16_t *input = (const int16_t *)source_pcm;
    for (uint32_t frame = 0; frame < converted_frames; ++frame) {
        uint64_t scaled = (uint64_t)frame * (uint32_t)source_rate;
        uint32_t first = (uint32_t)(scaled / MC2_AUDIO_RATE);
        uint32_t fraction = (uint32_t)(scaled % MC2_AUDIO_RATE);
        if (first >= source_frames)
            first = source_frames - 1;
        uint32_t second = first + 1 < source_frames ? first + 1 : first;
        for (unsigned channel = 0; channel < 2; ++channel) {
            unsigned source_channel = source_channels == 1 ? 0 : channel;
            int32_t a = input[(size_t)first * source_channels + source_channel];
            int32_t b = input[(size_t)second * source_channels + source_channel];
            converted[(size_t)frame * 2 + channel] = (int16_t)(
                (a * (int32_t)(MC2_AUDIO_RATE - fraction) +
                 b * (int32_t)fraction) / MC2_AUDIO_RATE);
        }
    }
    free(source_pcm);

    *output_pcm = converted;
    *output_frames = converted_frames;
    *output_bytes = converted_bytes;
    return 1;
}

static int decode_wav_48k(const char *path, int16_t **output_pcm,
                          uint32_t *output_frames, uint32_t *output_bytes) {
    FILE *file = fopen_soloader(path, "rb");
    if (!file) {
        port_trace("AUDIO: WAV open failed path=%s", path);
        return 0;
    }

    unsigned char header[12];
    if (fread_soloader(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        port_trace("AUDIO: invalid WAV header path=%s", path);
        fclose_soloader(file);
        return 0;
    }

    unsigned channels = 0;
    unsigned sample_rate = 0;
    unsigned bits_per_sample = 0;
    unsigned format = 0;
    unsigned block_align = 0;
    unsigned samples_per_block = 0;
    uint32_t data_size = 0;
    long data_offset = -1;
    unsigned char chunk[40];
    for (;;) {
        unsigned char chunk_header[8];
        if (fread_soloader(chunk_header, 1, sizeof(chunk_header), file) !=
            sizeof(chunk_header)) {
            break;
        }
        uint32_t chunk_size = read_u32_le(chunk_header + 4);
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            size_t copied = chunk_size < sizeof(chunk) ? chunk_size : sizeof(chunk);
            if (copied < 16 || fread_soloader(chunk, 1, copied, file) != copied) {
                break;
            }
            if (chunk_size > copied &&
                fseek_soloader(file, (long)(chunk_size - copied), SEEK_CUR) != 0) {
                break;
            }
            format = read_u16_le(chunk);
            channels = read_u16_le(chunk + 2);
            sample_rate = read_u32_le(chunk + 4);
            block_align = read_u16_le(chunk + 12);
            bits_per_sample = read_u16_le(chunk + 14);
            if (chunk_size >= 20) samples_per_block = read_u16_le(chunk + 18);
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_offset = ftell_soloader(file);
            data_size = chunk_size;
            break;
        } else if (fseek_soloader(file, (long)(chunk_size + (chunk_size & 1)),
                                  SEEK_CUR) != 0) {
            break;
        }
    }

    if (data_offset < 0 || !channels || !sample_rate ||
        (channels != 1 && channels != 2) ||
        (format != 1 && format != 17) ||
        (format == 1 && bits_per_sample != 16) ||
        (format == 17 && (bits_per_sample != 4 || !block_align ||
                          samples_per_block < 2)) ||
        data_size < channels * sizeof(int16_t) ||
        data_size > MC2_AUDIO_ASSET_LIMIT) {
        port_trace("AUDIO: unsupported WAV layout path=%s channels=%u rate=%u "
                   "bits=%u format=%u block=%u samples=%u data=%u", path,
                   channels, sample_rate, bits_per_sample, format,
                   block_align, samples_per_block, data_size);
        fclose_soloader(file);
        return 0;
    }

    if (fseek_soloader(file, data_offset, SEEK_SET) != 0) {
        fclose_soloader(file);
        return 0;
    }

    uint32_t source_frames = 0;
    uint32_t source_bytes = 0;
    int16_t *source_pcm = NULL;
    if (format == 1) {
        source_frames = data_size / (channels * sizeof(int16_t));
        source_bytes = source_frames * channels * sizeof(int16_t);
        source_pcm = (int16_t *)malloc(source_bytes);
        if (!source_pcm || fread_soloader(source_pcm, 1, source_bytes, file) !=
                             source_bytes) {
            free(source_pcm);
            fclose_soloader(file);
            port_trace("AUDIO: WAV data read failed path=%s bytes=%u", path,
                       source_bytes);
            return 0;
        }
    } else {
        static const int index_table[16] = {
            -1, -1, -1, -1, 2, 4, 6, 8,
            -1, -1, -1, -1, 2, 4, 6, 8,
        };
        static const int step_table[89] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
            31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
            118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
            371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
            1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
            2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
            7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
            16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
        };
        uint32_t block_count = (data_size + block_align - 1) / block_align;
        uint64_t max_frames = (uint64_t)block_count * samples_per_block;
        uint64_t max_bytes = max_frames * channels * sizeof(int16_t);
        if (!max_frames || max_bytes > MC2_AUDIO_ASSET_LIMIT) {
            fclose_soloader(file);
            port_trace("AUDIO: IMA WAV too large path=%s bytes=%u", path,
                       (unsigned)max_bytes);
            return 0;
        }
        source_pcm = (int16_t *)malloc((size_t)max_bytes);
        unsigned char *compressed = (unsigned char *)malloc(data_size);
        if (!source_pcm || !compressed ||
            fread_soloader(compressed, 1, data_size, file) != data_size) {
            free(source_pcm);
            free(compressed);
            fclose_soloader(file);
            port_trace("AUDIO: IMA WAV data read failed path=%s bytes=%u", path,
                       data_size);
            return 0;
        }

        uint32_t compressed_offset = 0;
        while (compressed_offset + block_align <= data_size) {
            const unsigned char *block = compressed + compressed_offset;
            int predictor[2] = {0, 0};
            int step_index[2] = {0, 0};
            for (unsigned channel = 0; channel < channels; ++channel) {
                size_t header = channel * 4;
                predictor[channel] = (int16_t)read_u16_le(block + header);
                step_index[channel] = block[header + 2];
                if (step_index[channel] > 88) step_index[channel] = 88;
            }
            uint32_t block_frames = 1;
            for (unsigned channel = 0; channel < channels; ++channel)
                source_pcm[(size_t)source_frames * channels + channel] =
                    (int16_t)predictor[channel];

            size_t payload = channels * 4;
            while (block_frames < samples_per_block &&
                   payload + channels * 4 <= block_align) {
                uint32_t group_start = block_frames;
                for (unsigned channel = 0; channel < channels; ++channel) {
                    const unsigned char *group = block + payload + channel * 4;
                    uint32_t sample = 0;
                    for (unsigned byte = 0; byte < 4 &&
                         sample < 8 && group_start + sample < samples_per_block;
                         ++byte) {
                        unsigned packed = group[byte];
                        for (unsigned nibble = 0; nibble < 2 && sample < 8 &&
                             group_start + sample < samples_per_block;
                             ++nibble, ++sample) {
                            unsigned code = nibble ? packed >> 4 : packed & 15;
                            int step = step_table[step_index[channel]];
                            int delta = step >> 3;
                            if (code & 1) delta += step >> 2;
                            if (code & 2) delta += step >> 1;
                            if (code & 4) delta += step;
                            predictor[channel] += (code & 8) ? -delta : delta;
                            if (predictor[channel] > 32767) predictor[channel] = 32767;
                            if (predictor[channel] < -32768) predictor[channel] = -32768;
                            step_index[channel] += index_table[code];
                            if (step_index[channel] < 0) step_index[channel] = 0;
                            if (step_index[channel] > 88) step_index[channel] = 88;
                            source_pcm[(size_t)(source_frames + group_start +
                                                sample) * channels + channel] =
                                (int16_t)predictor[channel];
                        }
                    }
                }
                payload += channels * 4;
                block_frames += 8;
            }
            if (block_frames > samples_per_block)
                block_frames = samples_per_block;
            source_frames += block_frames;
            compressed_offset += block_align;
        }
        source_bytes = source_frames * channels * sizeof(int16_t);
        free(compressed);
    }
    fclose_soloader(file);

    int decoded = resample_pcm_48k(source_pcm, source_frames,
                                   (int)sample_rate, (int)channels,
                                   output_pcm, output_frames, output_bytes);
    free(source_pcm);
    if (!decoded) {
        port_trace("AUDIO: WAV conversion failed path=%s", path);
    }
    return decoded;
}

static int decode_audio_48k(const char *path, int16_t **output_pcm,
                            uint32_t *output_frames, uint32_t *output_bytes) {
    const char *extension = strrchr(path ? path : "", '.');
    if (extension && !strcasecmp(extension, ".wav")) {
        return decode_wav_48k(path, output_pcm, output_frames, output_bytes);
    }
    return decode_ogg_48k(path, output_pcm, output_frames, output_bytes);
}

static int asset_in_use_locked(Mc2AudioAsset *asset) {
    if (music_voice.active && music_voice.asset == asset)
        return 1;
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].active && sound_voices[i].asset == asset)
            return 1;
    }
    return 0;
}

static void release_pcm_locked(Mc2AudioAsset *asset) {
    if (!asset->pcm)
        return;
    free(asset->pcm);
    asset->pcm = NULL;
    asset->frames = 0;
    if (audio_cache_bytes >= asset->bytes)
        audio_cache_bytes -= asset->bytes;
    else
        audio_cache_bytes = 0;
    asset->bytes = 0;
}

static void reserve_cache_locked(uint32_t required) {
    while (audio_cache_bytes + required > MC2_AUDIO_CACHE_LIMIT) {
        Mc2AudioAsset *oldest = NULL;
        for (unsigned kind = 0; kind < 2; ++kind) {
            Mc2AudioAsset *assets = kind ? music_assets : sound_assets;
            for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i) {
                Mc2AudioAsset *candidate = &assets[i];
                if (!candidate->pcm || asset_in_use_locked(candidate))
                    continue;
                if (!oldest || candidate->last_used < oldest->last_used)
                    oldest = candidate;
            }
        }
        if (!oldest)
            break;
        release_pcm_locked(oldest);
    }
}

static void start_music_voice_locked(Mc2AudioAsset *asset, int loop) {
    music_voice = (Mc2AudioVoice){
        .asset = asset, .position = 0,
        .loops = loop ? -1 : 0,
        .volume = 32768, .active = 1,
    };
    asset->last_used = ++audio_clock;
}

static int grow_sound_voices_locked(void) {
    unsigned old_capacity = sound_voice_capacity;
    unsigned new_capacity = old_capacity ? old_capacity * 2 :
                                          MC2_AUDIO_INITIAL_VOICES;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(*sound_voices)) {
        ++audio_stats_voice_growth_failures;
        return 0;
    }

    Mc2AudioVoice *grown = realloc(
        sound_voices, (size_t)new_capacity * sizeof(*sound_voices));
    if (!grown) {
        ++audio_stats_voice_growth_failures;
        port_trace("AUDIO: could not grow dynamic voices from %u to %u",
                   old_capacity, new_capacity);
        return 0;
    }
    memset(grown + old_capacity, 0,
           (size_t)(new_capacity - old_capacity) * sizeof(*grown));
    sound_voices = grown;
    sound_voice_capacity = new_capacity;
    port_trace("AUDIO: dynamic voice pool grew from %u to %u",
               old_capacity, new_capacity);
    return 1;
}

static void start_sound_voice_locked(Mc2AudioAsset *asset, int loop,
                                     int volume) {
    unsigned selected = sound_voice_capacity;
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (!sound_voices[i].active) {
            selected = i;
            break;
        }
    }
    if (selected == sound_voice_capacity) {
        unsigned first_new_slot = sound_voice_capacity;
        if (grow_sound_voices_locked())
            selected = first_new_slot;
    }

    if (selected == sound_voice_capacity) {
        /* Allocation failure is the only remaining limit.  Keep the game
         * running by replacing the least disruptive voice instead of losing
         * the new request or exhausting the process heap. */
        /* Prefer replacing a one-shot that is already close to its end.  The
         * old position-only heuristic could cut a long dialogue/ambient clip
         * merely because it had played for longer, producing a click and
         * making that sound disappear during busy firefights. */
        uint32_t least_remaining = UINT32_MAX;
        for (unsigned i = 0; i < sound_voice_capacity; ++i) {
            Mc2AudioVoice *voice = &sound_voices[i];
            if (voice->loops != 0 || !voice->asset)
                continue;
            uint32_t remaining = voice->position < voice->asset->frames ?
                voice->asset->frames - voice->position : 0;
            if (remaining < least_remaining) {
                least_remaining = remaining;
                selected = i;
            }
        }
        /* All voices are loops: preserve the loudest ambience and replace the
         * quietest loop. */
        if (selected == sound_voice_capacity) {
            if (!sound_voice_capacity)
                return;
            int quietest_gain = 0x7fffffff;
            selected = 0;
            for (unsigned i = 0; i < sound_voice_capacity; ++i) {
                Mc2AudioVoice *voice = &sound_voices[i];
                int gain = voice->volume;
                if (voice->asset)
                    gain = (gain * voice->asset->volume) >> 15;
                if (gain < quietest_gain) {
                    quietest_gain = gain;
                    selected = i;
                }
            }
        }
    }
    sound_voices[selected] = (Mc2AudioVoice){
        .asset = asset, .position = 0,
        .loops = loop ? -1 : 0,
        .volume = volume, .active = 1,
    };
    asset->last_used = ++audio_clock;
}

static Mc2AudioAsset *take_decode_request_locked(int is_music) {
    Mc2AudioAsset *assets = is_music ? music_assets : sound_assets;
    for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i) {
        Mc2AudioAsset *asset = &assets[i];
        if (!asset->valid || asset->pcm || asset->decoding ||
            !asset->decode_requested)
            continue;
        asset->decode_requested = 0;
        asset->decoding = 1;
        return asset;
    }
    return NULL;
}

/* Full Vorbis decoding can take several seconds for a level music track on
 * the Vita.  Never perform it from a JNI play callback: that callback runs on
 * the renderer thread.  Dedicated music and sound workers keep long music
 * decoding from delaying short dialogue/SFX requests. */
static void *audio_decode_worker(void *argument) {
    const int is_music = (int)(uintptr_t)argument;
    pthread_cond_t *condition = is_music ? &music_decode_condition :
                                           &sound_decode_condition;
    SceUID thread_id = sceKernelGetThreadId();
    int priority_result = sceKernelChangeThreadPriority(thread_id, 160);
    int affinity_result = sceKernelChangeThreadCpuAffinityMask(thread_id,
                                                                0x20000);
    port_trace("AUDIO: %s decoder scheduling priority_result=%d "
               "affinity_result=0x%x",
               is_music ? "music" : "sound", priority_result,
               affinity_result);
    for (;;) {
        char path[256];
        uint32_t generation;
        Mc2AudioAsset *asset;

        pthread_mutex_lock(&audio_mutex);
        while (!(asset = take_decode_request_locked(is_music)))
            pthread_cond_wait(condition, &audio_mutex);
        snprintf(path, sizeof(path), "%s", asset->path);
        generation = asset->generation;
        pthread_mutex_unlock(&audio_mutex);

        int16_t *pcm = NULL;
        uint32_t frames = 0;
        uint32_t bytes = 0;
        int decoded = decode_audio_48k(path, &pcm, &frames, &bytes);

        int started_music = 0;
        int started_sound = 0;
        uint32_t cache_kb = 0;
        pthread_mutex_lock(&audio_mutex);
        if (asset->valid && asset->generation == generation &&
            !strcmp(asset->path, path)) {
            asset->decoding = 0;
            if (decoded && !asset->pcm) {
                /* At a music transition the previous track must become
                 * evictable before reserving room for the new decoded PCM. */
                if (asset->pending_music && music_voice.asset != asset)
                    music_voice.active = 0;
                reserve_cache_locked(bytes);
                asset->pcm = pcm;
                asset->frames = frames;
                asset->bytes = bytes;
                asset->last_used = ++audio_clock;
                audio_cache_bytes += bytes;
                cache_kb = audio_cache_bytes / 1024;
                pcm = NULL;
                if (asset->pending_music) {
                    start_music_voice_locked(asset, asset->pending_loop);
                    asset->pending_music = 0;
                    started_music = 1;
                }
                if (asset->pending_sound) {
                    start_sound_voice_locked(asset, asset->pending_loop,
                                             asset->pending_volume);
                    asset->pending_sound = 0;
                    started_sound = 1;
                }
            } else if (!decoded) {
                asset->decode_failed = 1;
                asset->pending_music = 0;
                asset->pending_sound = 0;
            }
        }
        pthread_mutex_unlock(&audio_mutex);
        free(pcm);

        if (decoded) {
            port_trace("AUDIO: decoded async %s kind=%s frames=%u cache=%uKB",
                       path, is_music ? "music" : "sound", frames, cache_kb);
            if (started_music)
                port_trace("AUDIO: async music started path=%s", path);
            if (started_sound)
                port_trace("AUDIO: async sound started path=%s", path);
        } else {
            port_trace("AUDIO: async decode failed path=%s", path);
        }
    }
    return NULL;
}

static int start_decode_thread(int is_music) {
    int *started = is_music ? &music_decode_thread_started :
                              &sound_decode_thread_started;
    pthread_mutex_lock(&audio_mutex);
    if (*started) {
        pthread_mutex_unlock(&audio_mutex);
        return 1;
    }
    *started = 1;
    pthread_mutex_unlock(&audio_mutex);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024);
    unsigned worker_count = is_music ? 1 : MC2_SOUND_DECODE_WORKERS;
    unsigned created = 0;
    int last_result = 0;
    for (unsigned i = 0; i < worker_count; ++i) {
        pthread_t *thread = is_music ? &music_decode_thread :
                                       &sound_decode_threads[i];
        last_result = pthread_create(thread, &attributes, audio_decode_worker,
                                     (void *)(uintptr_t)is_music);
        if (!last_result)
            ++created;
    }
    pthread_attr_destroy(&attributes);
    if (!created) {
        pthread_mutex_lock(&audio_mutex);
        *started = 0;
        pthread_mutex_unlock(&audio_mutex);
        port_trace("AUDIO: %s decode thread failed=%d",
                   is_music ? "music" : "sound", last_result);
        return 0;
    }
    port_trace("AUDIO: %s decode workers started=%u requested=%u",
               is_music ? "music" : "sound", created, worker_count);
    return 1;
}

static int request_decode_locked(Mc2AudioAsset *asset, int is_music) {
    if (!asset->valid || asset->decode_failed)
        return 0;
    if (!asset->pcm && !asset->decoding && !asset->decode_requested) {
        asset->decode_requested = 1;
        pthread_cond_signal(is_music ? &music_decode_condition :
                                       &sound_decode_condition);
    }
    return 1;
}

static void mix_voice_locked(Mc2AudioVoice *voice, int64_t *mix,
                             unsigned frames, int group_gain) {
    if (!voice->active || voice->paused || !voice->asset ||
        !voice->asset->pcm || !voice->asset->frames)
        return;

    for (unsigned out = 0; out < frames; ++out) {
        if (voice->position >= voice->asset->frames) {
            if (voice->loops < 0 || voice->loops > 0) {
                if (voice->loops > 0)
                    --voice->loops;
                voice->position = 0;
            } else {
                voice->active = 0;
                break;
            }
        }
        int gain = (voice->volume * voice->asset->volume) >> 15;
        gain = (gain * group_gain) >> 15;
        const int16_t *sample = voice->asset->pcm + voice->position * 2;
        mix[out * 2] += (sample[0] * gain) >> 15;
        mix[out * 2 + 1] += (sample[1] * gain) >> 15;
        ++voice->position;
    }
}

static unsigned active_sound_voices_locked(void) {
    unsigned active = 0;
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        Mc2AudioVoice *voice = &sound_voices[i];
        if (voice->active && !voice->paused && voice->asset &&
            voice->asset->pcm && voice->asset->frames)
            ++active;
    }
    return active;
}

/*
 * Convert the 64-bit mix to Vita's 16-bit output with a short-lookahead peak
 * limiter.  v20 changed the gain only once per 2048-frame (42 ms) block and
 * also scaled every effect according to the number of active voices.  Both
 * operations produced audible level steps during a firefight.  Here every
 * voice keeps its requested gain and only genuine peaks are controlled, in
 * 32-frame (0.67 ms) windows.  Attack is immediate before the peak and the
 * release is gradual, so there is no long block-wide pumping or hard clip.
 */
static void write_limited_output(const int64_t *mix, int16_t *output,
                                 int *limiter_gain, uint64_t *peak_out,
                                 unsigned *minimum_gain_out,
                                 unsigned *limited_chunks_out) {
    uint64_t maximum_peak = 0;
    unsigned minimum_gain = 32768;
    unsigned limited_chunks = 0;

    for (unsigned base = 0; base < MC2_AUDIO_FRAMES;
         base += MC2_LIMITER_CHUNK_FRAMES) {
        unsigned end = base + MC2_LIMITER_CHUNK_FRAMES;
        if (end > MC2_AUDIO_FRAMES)
            end = MC2_AUDIO_FRAMES;

        uint64_t chunk_peak = 0;
        for (unsigned frame = base; frame < end; ++frame) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                int64_t value = mix[frame * 2 + channel];
                uint64_t magnitude = value < 0 ?
                    (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
                if (magnitude > chunk_peak)
                    chunk_peak = magnitude;
            }
        }
        if (chunk_peak > maximum_peak)
            maximum_peak = chunk_peak;

        int target_gain = 32768;
        if (chunk_peak > MC2_LIMITER_TARGET_PEAK) {
            target_gain = (int)(((uint64_t)MC2_LIMITER_TARGET_PEAK * 32768u) /
                                chunk_peak);
            ++limited_chunks;
        }

        if (target_gain < *limiter_gain) {
            /* The whole small window is already available, which gives the
             * limiter enough lookahead to reduce gain before its peak. */
            *limiter_gain = target_gain;
        } else if (target_gain > *limiter_gain) {
            unsigned frames = end - base;
            int release = ((32768 - *limiter_gain) * (int)frames +
                           ((1 << MC2_LIMITER_RELEASE_SHIFT) - 1)) >>
                          MC2_LIMITER_RELEASE_SHIFT;
            if (release < 1)
                release = 1;
            *limiter_gain += release;
            if (*limiter_gain > target_gain)
                *limiter_gain = target_gain;
        }
        if ((unsigned)*limiter_gain < minimum_gain)
            minimum_gain = (unsigned)*limiter_gain;

        for (unsigned frame = base; frame < end; ++frame) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                unsigned index = frame * 2 + channel;
                int64_t sample = (mix[index] * *limiter_gain) >> 15;
                if (sample > 32767)
                    sample = 32767;
                else if (sample < -32768)
                    sample = -32768;
                output[index] = (int16_t)sample;
            }
        }
    }

    *peak_out = maximum_peak;
    *minimum_gain_out = minimum_gain;
    *limited_chunks_out = limited_chunks;
}

static void *audio_output_thread(void *unused) {
    (void)unused;
    SceUID thread_id = sceKernelGetThreadId();
    int priority_result = sceKernelChangeThreadPriority(thread_id, 32);
    int affinity_result = sceKernelChangeThreadCpuAffinityMask(thread_id,
                                                                0x40000);
    /* FMOD's OpenSL compatibility path owns the MAIN port for short effects.
     * Background music must use Vita's dedicated BGM port; opening another
     * MAIN stream makes the two producers fight over the same output and
     * produces the audible interference seen on the menu. */
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                   MC2_AUDIO_FRAMES, MC2_AUDIO_RATE,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    port_trace("AUDIO: BGM port=%d frames=%d rate=%d voices=dynamic(%u) "
               "priority_result=%d affinity_result=0x%x", port,
               MC2_AUDIO_FRAMES, MC2_AUDIO_RATE,
               sound_voice_capacity, priority_result, affinity_result);
    if (port < 0)
        return NULL;

    int64_t mix[MC2_AUDIO_FRAMES * 2];
    unsigned buffer = 0;
    int limiter_gain = 32768;
    uint64_t previous_output_time = 0;
    for (;;) {
        memset(mix, 0, sizeof(mix));
        pthread_mutex_lock(&audio_mutex);
        if (!audio_paused) {
            unsigned active_sounds = active_sound_voices_locked();
            if (active_sounds > audio_stats_max_voices)
                audio_stats_max_voices = active_sounds;

            mix_voice_locked(&music_voice, mix, MC2_AUDIO_FRAMES, 32768);
            for (unsigned i = 0; i < sound_voice_capacity; ++i)
                mix_voice_locked(&sound_voices[i], mix, MC2_AUDIO_FRAMES,
                                 32768);
        }
        pthread_mutex_unlock(&audio_mutex);

        uint64_t peak = 0;
        unsigned minimum_gain = 32768;
        unsigned limited_chunks = 0;
        int16_t *output = audio_output[buffer];
        write_limited_output(mix, output, &limiter_gain, &peak,
                             &minimum_gain, &limited_chunks);

        pthread_mutex_lock(&audio_mutex);
        if (peak > audio_stats_max_peak)
            audio_stats_max_peak = peak;
        if (minimum_gain < audio_stats_min_gain)
            audio_stats_min_gain = minimum_gain;
        audio_stats_limited_chunks += limited_chunks;
        pthread_mutex_unlock(&audio_mutex);

        int result = sceAudioOutOutput(port, output);
        uint64_t output_time = sceKernelGetSystemTimeWide();
        if (previous_output_time &&
            output_time - previous_output_time > 55000) {
            pthread_mutex_lock(&audio_mutex);
            ++audio_stats_late_buffers;
            pthread_mutex_unlock(&audio_mutex);
        }
        previous_output_time = output_time;
        if (result < 0) {
            port_trace("AUDIO: sceAudioOutOutput failed=0x%x", result);
            break;
        }
        buffer ^= 1;
    }
    sceAudioOutReleasePort(port);
    return NULL;
}

void mc2_audio_log_stats(void) {
    pthread_mutex_lock(&audio_mutex);
    unsigned max_voices = audio_stats_max_voices;
    uint64_t max_peak = audio_stats_max_peak;
    unsigned voice_capacity = sound_voice_capacity;
    unsigned growth_failures = audio_stats_voice_growth_failures;
    unsigned min_gain = audio_stats_min_gain;
    unsigned limited_chunks = audio_stats_limited_chunks;
    unsigned late_buffers = audio_stats_late_buffers;
    audio_stats_max_voices = 0;
    audio_stats_max_peak = 0;
    audio_stats_min_gain = 32768;
    audio_stats_limited_chunks = 0;
    audio_stats_late_buffers = 0;
    audio_stats_voice_growth_failures = 0;
    pthread_mutex_unlock(&audio_mutex);
    port_trace("AUDIOSTATS: max_voices=%u capacity=%u peak=%llu "
               "min_gain_q15=%u protected_chunks=%u late=%u "
               "growth_failures=%u",
               max_voices, voice_capacity, (unsigned long long)max_peak,
               min_gain, limited_chunks, late_buffers, growth_failures);
}

static void start_audio_thread(void) {
    pthread_mutex_lock(&audio_mutex);
    if (audio_thread_started) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (!sound_voice_capacity && !grow_sound_voices_locked()) {
        pthread_mutex_unlock(&audio_mutex);
        port_trace("AUDIO: initial dynamic voice allocation failed");
        return;
    }
    audio_thread_started = 1;
    pthread_mutex_unlock(&audio_mutex);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 128 * 1024);
    int result = pthread_create(&audio_thread, &attributes,
                                audio_output_thread, NULL);
    pthread_attr_destroy(&attributes);
    if (result) {
        pthread_mutex_lock(&audio_mutex);
        audio_thread_started = 0;
        pthread_mutex_unlock(&audio_mutex);
        port_trace("AUDIO: pthread_create failed=%d", result);
    }
}

static void load_asset(Mc2AudioAsset *assets, int id, const char *name,
                       const char *kind) {
    if (!valid_id(id) || !name || !name[0]) {
        port_trace("AUDIO: invalid %s load id=%d name=%s", kind, id,
                   name ? name : "(null)");
        return;
    }
    char path[256];
    make_audio_path(name, path);
    pthread_mutex_lock(&audio_mutex);
    Mc2AudioAsset *asset = &assets[id];
    if (asset->valid && !strcmp(asset->path, path)) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    if (asset_in_use_locked(asset)) {
        if (music_voice.asset == asset)
            music_voice.active = 0;
        for (unsigned i = 0; i < sound_voice_capacity; ++i) {
            if (sound_voices[i].asset == asset)
                sound_voices[i].active = 0;
        }
    }
    uint32_t generation = asset->generation + 1;
    release_pcm_locked(asset);
    memset(asset, 0, sizeof(*asset));
    asset->generation = generation;
    snprintf(asset->path, sizeof(asset->path), "%s", path);
    asset->valid = 1;
    asset->volume = 32768;
    pthread_mutex_unlock(&audio_mutex);
    port_trace("AUDIO: load %s id=%d path=%s", kind, id, path);
}

void mc2_audio_load_music(int id, const char *name) {
    load_asset(music_assets, id, name, "music");
}

void mc2_audio_load_sound(int id, const char *name) {
    load_asset(sound_assets, id, name, "sound");
}

void mc2_audio_play_music(int id, int loop) {
    if (!valid_id(id) || !start_decode_thread(1)) {
        port_trace("AUDIO: play music failed id=%d loop=%d", id, loop);
        return;
    }
    start_audio_thread();
    pthread_mutex_lock(&audio_mutex);
    Mc2AudioAsset *asset = &music_assets[id];
    if (!request_decode_locked(asset, 1)) {
        pthread_mutex_unlock(&audio_mutex);
        port_trace("AUDIO: play music unavailable id=%d loop=%d", id, loop);
        return;
    }
    int ready = asset->pcm != NULL;
    if (ready) {
        start_music_voice_locked(asset, loop);
    } else {
        asset->pending_music = 1;
        asset->pending_loop = loop;
    }
    pthread_mutex_unlock(&audio_mutex);
    port_trace("AUDIO: play music id=%d loop=%d state=%s", id, loop,
               ready ? "ready" : "queued");
}

void mc2_audio_play_sound(int id, int loop, const char *fallback_name,
                          float volume) {
    if (!valid_id(id))
        return;
    if (!sound_assets[id].valid && fallback_name && fallback_name[0])
        mc2_audio_load_sound(id, fallback_name);
    if (!start_decode_thread(0)) {
        port_trace("AUDIO: play sound failed id=%d loop=%d", id, loop);
        return;
    }
    start_audio_thread();
    pthread_mutex_lock(&audio_mutex);
    Mc2AudioAsset *asset = &sound_assets[id];
    if (!request_decode_locked(asset, 0)) {
        pthread_mutex_unlock(&audio_mutex);
        port_trace("AUDIO: play sound unavailable id=%d loop=%d", id, loop);
        return;
    }
    int fixed_volume = clamp_volume(volume);
    if (asset->pcm) {
        start_sound_voice_locked(asset, loop, fixed_volume);
    } else {
        /* Coalesce repeated requests for an asset that is still decoding.
         * Replaying every old request together after the decoder finishes
         * would produce an incorrect burst.  Once the first copy is ready,
         * later calls use the dynamically growing mixer. */
        asset->pending_sound = 1;
        asset->pending_loop = loop;
        asset->pending_volume = fixed_volume;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_pause_music(int id) {
    pthread_mutex_lock(&audio_mutex);
    if (music_voice.active && (!valid_id(id) ||
        music_voice.asset == &music_assets[id]))
        music_voice.paused = 1;
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_resume_music(int id) {
    pthread_mutex_lock(&audio_mutex);
    if (music_voice.active && (!valid_id(id) ||
        music_voice.asset == &music_assets[id]))
        music_voice.paused = 0;
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_stop_music(int id) {
    pthread_mutex_lock(&audio_mutex);
    if (music_voice.active && (!valid_id(id) ||
        music_voice.asset == &music_assets[id]))
        music_voice.active = 0;
    if (valid_id(id))
        music_assets[id].pending_music = 0;
    else {
        for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i)
            music_assets[i].pending_music = 0;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_pause_sound(int id) {
    pthread_mutex_lock(&audio_mutex);
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].active && (!valid_id(id) ||
            sound_voices[i].asset == &sound_assets[id]))
            sound_voices[i].paused = 1;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_resume_sound(int id) {
    pthread_mutex_lock(&audio_mutex);
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].active && (!valid_id(id) ||
            sound_voices[i].asset == &sound_assets[id]))
            sound_voices[i].paused = 0;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_stop_sound(int id) {
    pthread_mutex_lock(&audio_mutex);
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].active && (!valid_id(id) ||
            sound_voices[i].asset == &sound_assets[id]))
            sound_voices[i].active = 0;
    }
    if (valid_id(id))
        sound_assets[id].pending_sound = 0;
    else {
        for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i)
            sound_assets[i].pending_sound = 0;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_pause_all(void) {
    pthread_mutex_lock(&audio_mutex);
    audio_paused = 1;
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_resume_all(void) {
    pthread_mutex_lock(&audio_mutex);
    audio_paused = 0;
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_stop_all(void) {
    pthread_mutex_lock(&audio_mutex);
    music_voice.active = 0;
    for (unsigned i = 0; i < sound_voice_capacity; ++i)
        sound_voices[i].active = 0;
    for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i) {
        music_assets[i].pending_music = 0;
        sound_assets[i].pending_sound = 0;
    }
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_set_volume(int id, float volume) {
    if (!valid_id(id))
        return;
    int fixed = clamp_volume(volume);
    pthread_mutex_lock(&audio_mutex);
    if (music_assets[id].valid)
        music_assets[id].volume = fixed;
    if (sound_assets[id].valid)
        sound_assets[id].volume = fixed;
    pthread_mutex_unlock(&audio_mutex);
}

static void unload_asset(Mc2AudioAsset *assets, int id) {
    if (!valid_id(id))
        return;
    pthread_mutex_lock(&audio_mutex);
    Mc2AudioAsset *asset = &assets[id];
    if (music_voice.asset == asset)
        music_voice.active = 0;
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].asset == asset)
            sound_voices[i].active = 0;
    }
    uint32_t generation = asset->generation + 1;
    release_pcm_locked(asset);
    memset(asset, 0, sizeof(*asset));
    asset->generation = generation;
    pthread_mutex_unlock(&audio_mutex);
}

void mc2_audio_unload_music(int id) {
    unload_asset(music_assets, id);
}

void mc2_audio_unload_sound(int id) {
    unload_asset(sound_assets, id);
}

void mc2_audio_reset_sound(int id) {
    mc2_audio_stop_sound(id);
}

int mc2_audio_is_sound_loaded(int id) {
    if (!valid_id(id))
        return 0;
    pthread_mutex_lock(&audio_mutex);
    int result = sound_assets[id].valid;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

int mc2_audio_is_music_playing(int id) {
    pthread_mutex_lock(&audio_mutex);
    int result = music_voice.active && !music_voice.paused &&
        (!valid_id(id) || music_voice.asset == &music_assets[id]);
    if (!result) {
        if (valid_id(id)) {
            result = music_assets[id].pending_music;
        } else {
            for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i) {
                if (music_assets[i].pending_music) {
                    result = 1;
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

int mc2_audio_is_sound_playing(int id) {
    int result = 0;
    pthread_mutex_lock(&audio_mutex);
    for (unsigned i = 0; i < sound_voice_capacity; ++i) {
        if (sound_voices[i].active && !sound_voices[i].paused &&
            (!valid_id(id) || sound_voices[i].asset == &sound_assets[id])) {
            result = 1;
            break;
        }
    }
    if (!result) {
        if (valid_id(id)) {
            result = sound_assets[id].pending_sound;
        } else {
            for (unsigned i = 0; i < MC2_AUDIO_ASSETS; ++i) {
                if (sound_assets[i].pending_sound) {
                    result = 1;
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&audio_mutex);
    return result;
}

static int duration_ms(Mc2AudioAsset *asset) {
    pthread_mutex_lock(&audio_mutex);
    uint32_t frames = asset->frames;
    pthread_mutex_unlock(&audio_mutex);
    return (int)(((uint64_t)frames * 1000) / MC2_AUDIO_RATE);
}

int mc2_audio_get_music_duration(int id) {
    return valid_id(id) ? duration_ms(&music_assets[id]) : 0;
}

int mc2_audio_get_sound_duration(int id) {
    return valid_id(id) ? duration_ms(&sound_assets[id]) : 0;
}
