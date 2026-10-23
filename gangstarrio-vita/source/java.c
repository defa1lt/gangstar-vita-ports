#include "diagnostics.h"
#include "java_bridge.h"

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#include <psp2/audioout.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Java surface used by the original Gangstar Rio native library. The table
 * covers every JNI method reference recovered from the supported ARM binary;
 * FalsoJNI logging remains enabled to catch dynamic or obfuscated lookups. */

static int dummy_object;
static unsigned stub_calls;
static RioSetPathsCall set_paths_callback;
static jclass set_paths_class;
static int paths_ready;
static SceUID graphics_context_owner;

#define SHARED_VALUE_SLOTS 16
#define SHARED_KEY_SIZE 64
#define SHARED_VALUE_SIZE 192

typedef struct SharedValue {
    char key[SHARED_KEY_SIZE];
    char value[SHARED_VALUE_SIZE];
    int used;
} SharedValue;

static SharedValue shared_values[SHARED_VALUE_SLOTS];

#define AUDIO_GRAIN_FRAMES 1024
static int audio_port = -1;
static int audio_frequency = 44100;
static int audio_stereo = 1;
static int audio_play_state = 1;
static int audio_accumulated_frames;
static int16_t audio_accumulator[AUDIO_GRAIN_FRAMES * 2];

static const char resource_path[] = "ux0:data/gangstarrio/game/";
static const char files_path[] = "ux0:data/gangstarrio/files/";
static const char cache_path[] = "ux0:data/gangstarrio/cache/";

static void trace_stub(jmethodID id, const char *kind) {
    unsigned call = ++stub_calls;
    if (call <= 80 || call % 500 == 0) {
        LOG_INFO("Java %s stub: id=%d call=%u", kind, (int)id, call);
    }
}

static void method_void_stub(jmethodID id, va_list args) {
    (void)args;
    trace_stub(id, "void");
}

static void method_setup_paths(jmethodID id, va_list args) {
    (void)id;
    (void)args;

    if (!set_paths_callback || !set_paths_class) {
        LOG_ERROR("Java setupPaths: native callback is not configured");
        return;
    }

    /* GL2JNILib.setupPaths() creates these directories before it re-enters
     * the native setPaths method. Preserve that Java-side behavior. */
    (void)sceIoMkdir(resource_path, 0777);
    (void)sceIoMkdir(files_path, 0777);
    (void)sceIoMkdir(cache_path, 0777);

    jstring resource = jni->NewStringUTF(&jni, resource_path);
    jstring files = jni->NewStringUTF(&jni, files_path);
    jstring cache = jni->NewStringUTF(&jni, cache_path);
    if (!resource || !files || !cache) {
        LOG_ERROR("Java setupPaths: could not allocate path strings");
        return;
    }

    LOG_INFO("Java setupPaths: forwarding paths during App creation");
    diagnostics_flush();
    set_paths_callback(&jni, set_paths_class, resource, files, cache);
    paths_ready = 1;
    LOG_SUCCESS("Java setupPaths: native paths installed");
    diagnostics_flush();
}

static void method_set_view_settings(jmethodID id, va_list args) {
    (void)id;
    jint red = va_arg(args, jint);
    jint green = va_arg(args, jint);
    jint blue = va_arg(args, jint);
    jint alpha = va_arg(args, jint);
    jint depth = va_arg(args, jint);
    LOG_INFO("Java setViewSettings: rgba=%d/%d/%d/%d depth=%d",
             red, green, blue, alpha, depth);
}

static void audio_ensure_port(void) {
    if (audio_port >= 0) {
        return;
    }

    static const int supported_rates[] = {
        48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
    };
    int best_rate = 44100;
    int best_delta = 1 << 30;
    for (unsigned i = 0;
         i < sizeof(supported_rates) / sizeof(supported_rates[0]); ++i) {
        int delta = audio_frequency - supported_rates[i];
        if (delta < 0) {
            delta = -delta;
        }
        if (delta < best_delta) {
            best_delta = delta;
            best_rate = supported_rates[i];
        }
    }

    audio_port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_BGM,
        AUDIO_GRAIN_FRAMES,
        best_rate,
        audio_stereo ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
    LOG_INFO("Java AudioTrack: open %d Hz stereo=%d -> 0x%08X",
             best_rate, audio_stereo, audio_port);
    if (audio_port >= 0) {
        (void)sceAudioOutSetConfig(audio_port, -1, -1, -1);
    }
}

static void audio_output_pcm16(const int16_t *samples, int frames) {
    audio_ensure_port();
    if (audio_port < 0 || !samples) {
        return;
    }

    int channels = audio_stereo ? 2 : 1;
    while (frames > 0) {
        int take = AUDIO_GRAIN_FRAMES - audio_accumulated_frames;
        if (take > frames) {
            take = frames;
        }
        memcpy(&audio_accumulator[audio_accumulated_frames * channels], samples,
               (size_t)take * (size_t)channels * sizeof(*samples));
        audio_accumulated_frames += take;
        samples += take * channels;
        frames -= take;
        if (audio_accumulated_frames == AUDIO_GRAIN_FRAMES) {
            (void)sceAudioOutOutput(audio_port, audio_accumulator);
            audio_accumulated_frames = 0;
        }
    }
}

static jobject object_audio_track(jmethodID id, va_list args) {
    (void)id;
    jint stream = va_arg(args, jint);
    audio_frequency = va_arg(args, jint);
    jint channel_config = va_arg(args, jint);
    jint format = va_arg(args, jint);
    jint buffer_size = va_arg(args, jint);
    jint mode = va_arg(args, jint);
    audio_stereo = channel_config == 12 || channel_config == 3;
    audio_play_state = 2;
    LOG_INFO("Java AudioTrack: stream=%d rate=%d channels=%d format=%d "
             "buffer=%d mode=%d", stream, audio_frequency, channel_config,
             format, buffer_size, mode);
    return (jobject)&dummy_object;
}

static void method_audio_play(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    audio_play_state = 3;
}

static void method_audio_pause(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    audio_play_state = 2;
}

static void method_audio_stop(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    audio_play_state = 1;
    audio_accumulated_frames = 0;
}

static void method_audio_release(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    if (audio_port >= 0) {
        (void)sceAudioOutReleasePort(audio_port);
        audio_port = -1;
    }
    audio_play_state = 1;
    audio_accumulated_frames = 0;
}

static jobject object_dummy(jmethodID id, va_list args) {
    (void)args;
    trace_stub(id, "object");
    return (jobject)&dummy_object;
}

static jobject string_value(const char *value) {
    return (jobject)jni->NewStringUTF(&jni, value);
}

static jobject string_empty(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("");
}

static jobject string_phone_manufacturer(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    /* GloftG4HM.getPhoneManufacturer() lower-cases Build.MANUFACTURER. */
    return string_value("sony");
}

static jobject string_phone_firmware(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("4.4.2");
}

static jobject string_game_version(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("1.2.2b");
}

static jobject string_iso3_country(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("RUS");
}

static jobject string_iso3_language(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("rus");
}

static jobject string_mac(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("02:00:00:00:00:01");
}

static jobject string_user_agent(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("GangstarRio/1.2.2b (PlayStation Vita)");
}

static jobject string_resource_path(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value(resource_path);
}

static jobject string_files_path(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value(files_path);
}

static jobject string_cache_path(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value(cache_path);
}

static jobject string_main_obb(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("main.12030.com.gameloft.android.ANMP.GloftG4HM.obb");
}

static jobject string_patch_obb(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb");
}

static jobject string_package(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("com.gameloft.android.ANMP.GloftG4HM");
}

static jobject string_country(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("RU");
}

static jobject string_language(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("ru");
}

static jobject string_device_id(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("PSVITA-GSRV00001");
}

static jobject string_phone_model(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return string_value("pch-2000");
}

static jobject byte_array_missing(jmethodID id, va_list args) {
    (void)id;
    jstring name = va_arg(args, jstring);
    const char *chars = name ? jni->GetStringUTFChars(&jni, name, NULL) : NULL;
    LOG_INFO("Java getResource: %s", chars ? chars : "(null)");
    if (name && chars) {
        jni->ReleaseStringUTFChars(&jni, name, (char *)chars);
    }
    /* Android returns null here when a class-path resource is absent. Game
     * data is read through the native filesystem bridge instead. */
    return NULL;
}

static jobject byte_array_empty(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)jni->NewByteArray(&jni, 0);
}

static int shared_find(const char *key) {
    if (!key) {
        return -1;
    }
    for (int i = 0; i < SHARED_VALUE_SLOTS; ++i) {
        if (shared_values[i].used && strcmp(shared_values[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int shared_free_slot(void) {
    for (int i = 0; i < SHARED_VALUE_SLOTS; ++i) {
        if (!shared_values[i].used) {
            return i;
        }
    }
    return 0;
}

static jobject string_shared_value(jmethodID id, va_list args) {
    (void)id;
    jstring key_string = va_arg(args, jstring);
    const char *key = key_string
        ? jni->GetStringUTFChars(&jni, key_string, NULL) : NULL;
    int slot = shared_find(key);
    jobject result = string_value(slot >= 0 ? shared_values[slot].value : "");
    if (key_string && key) {
        jni->ReleaseStringUTFChars(&jni, key_string, (char *)key);
    }
    return result;
}

static jboolean boolean_shared_value(jmethodID id, va_list args) {
    (void)id;
    jstring key_string = va_arg(args, jstring);
    const char *key = key_string
        ? jni->GetStringUTFChars(&jni, key_string, NULL) : NULL;
    int slot = shared_find(key);
    jboolean result = slot >= 0 && shared_values[slot].value[0]
        ? JNI_TRUE : JNI_FALSE;
    if (key_string && key) {
        jni->ReleaseStringUTFChars(&jni, key_string, (char *)key);
    }
    return result;
}

static void method_set_shared_value(jmethodID id, va_list args) {
    (void)id;
    jstring key_string = va_arg(args, jstring);
    jstring value_string = va_arg(args, jstring);
    const char *key = key_string
        ? jni->GetStringUTFChars(&jni, key_string, NULL) : NULL;
    const char *value = value_string
        ? jni->GetStringUTFChars(&jni, value_string, NULL) : NULL;
    if (key && value) {
        int slot = shared_find(key);
        if (slot < 0) {
            slot = shared_free_slot();
        }
        snprintf(shared_values[slot].key, sizeof(shared_values[slot].key),
                 "%s", key);
        snprintf(shared_values[slot].value, sizeof(shared_values[slot].value),
                 "%s", value);
        shared_values[slot].used = 1;
    }
    if (key_string && key) {
        jni->ReleaseStringUTFChars(&jni, key_string, (char *)key);
    }
    if (value_string && value) {
        jni->ReleaseStringUTFChars(&jni, value_string, (char *)value);
    }
}

static void method_delete_shared_value(jmethodID id, va_list args) {
    (void)id;
    jstring key_string = va_arg(args, jstring);
    const char *key = key_string
        ? jni->GetStringUTFChars(&jni, key_string, NULL) : NULL;
    int slot = shared_find(key);
    if (slot >= 0) {
        memset(&shared_values[slot], 0, sizeof(shared_values[slot]));
    }
    if (key_string && key) {
        jni->ReleaseStringUTFChars(&jni, key_string, (char *)key);
    }
}

static jboolean boolean_false(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_FALSE;
}

static jboolean boolean_true(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_TRUE;
}

static jboolean boolean_set_current_context(jmethodID id, va_list args) {
    (void)id;
    int index = va_arg(args, int);
    SceUID thread = sceKernelGetThreadId();
    /* VitaGL has one persistent context. Rejecting extra reservations lets
     * the engine use its existing deferred graphics queue on the owner. */
    jboolean accepted = index == 0 && thread == graphics_context_owner;
    static unsigned calls;
    unsigned call = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (call <= 16 || call % 500 == 0)
        LOG_INFO("GL CONTEXT: request=%d thread=0x%x owner=0x%x accepted=%d",
                 index, thread, graphics_context_owner, accepted);
    return accepted;
}

static jint integer_zero(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 0;
}

static jint integer_true(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 1;
}

static jint integer_width(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 960;
}

static jint integer_height(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 544;
}

static jint integer_audio_sample_rate(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 44100;
}

static jint integer_audio_buffer_size(jmethodID id, va_list args) {
    (void)id;
    jint sample_rate = va_arg(args, jint);
    jint channel_config = va_arg(args, jint);
    jint format = va_arg(args, jint);
    LOG_INFO("Java AudioTrack.getMinBufferSize: %d/%d/%d",
             sample_rate, channel_config, format);
    return AUDIO_GRAIN_FRAMES * 4;
}

static jint integer_audio_play_state(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return audio_play_state;
}

static jint integer_audio_write(jmethodID id, va_list args) {
    (void)id;
    jbyteArray array = va_arg(args, jbyteArray);
    jint offset = va_arg(args, jint);
    jint size = va_arg(args, jint);
    JavaDynArray *dynamic_array = (JavaDynArray *)array;
    if (!dynamic_array || !dynamic_array->array || size <= 0 || offset < 0) {
        return 0;
    }
    const int channels = audio_stereo ? 2 : 1;
    const int16_t *samples = (const int16_t *)(
        (const uint8_t *)dynamic_array->array + offset);
    audio_output_pcm16(samples, size / ((int)sizeof(*samples) * channels));
    return size;
}

static jfloat float_memory_megabytes(jmethodID id, va_list args) {
    static int reported;
    (void)id;
    (void)args;
    if (!reported) {
        reported = 1;
        LOG_INFO("Java GetMemoryInfo: reporting 128 MB Vita budget");
    }
    /* Select the 2011 low-end GLES2 settings.  The 256 MB answer still picks
     * a medium tier whose 77 MiB city and decoded textures are a poor fit for
     * VitaGL's 96 MB video-memory pool. */
    return 128.0f;
}

static jlong current_time_millis(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jlong)(sceKernelGetProcessTimeWide() / 1000);
}

static jlong long_zero(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 0;
}

enum {
    MID_VOID = 1,
    MID_SETUP_PATHS,
    MID_OBJECT,
    MID_EMPTY,
    MID_RESOURCE_PATH,
    MID_FILES_PATH,
    MID_CACHE_PATH,
    MID_MAIN_OBB,
    MID_PATCH_OBB,
    MID_PACKAGE,
    MID_COUNTRY,
    MID_LANGUAGE,
    MID_DEVICE_ID,
    MID_FALSE,
    MID_TRUE,
    MID_SET_CURRENT_CONTEXT,
    MID_ZERO,
    MID_WIDTH,
    MID_HEIGHT,
    MID_CURRENT_TIME,
    MID_VIEW_SETTINGS,
    MID_RESOURCE_BYTES,
    MID_PHONE_MODEL,
    MID_MEMORY_INFO,
    MID_PHONE_MANUFACTURER,
    MID_PHONE_FIRMWARE,
    MID_GAME_VERSION,
    MID_ISO3_COUNTRY,
    MID_ISO3_LANGUAGE,
    MID_MAC,
    MID_USER_AGENT,
    MID_EMPTY_BYTES,
    MID_SHARED_VALUE,
    MID_SHARED_SET,
    MID_SHARED_DELETE,
    MID_SHARED_HAS,
    MID_AUDIO_SAMPLE_RATE,
    MID_AUDIO_BUFFER_SIZE,
    MID_AUDIO_PLAY_STATE,
    MID_AUDIO_WRITE,
    MID_LONG_ZERO,
    MID_AUDIO_INIT,
    MID_AUDIO_PLAY,
    MID_AUDIO_PAUSE,
    MID_AUDIO_STOP,
    MID_AUDIO_RELEASE,
};

NameToMethodID nameToMethodId[] = {
    {MID_OBJECT, "java/lang/Object/<init>", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "java/lang/String/<init>", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "java/io/File/<init>", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "android/os/Bundle/<init>", METHOD_TYPE_OBJECT},
    {MID_AUDIO_INIT, "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getApplicationContext", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getContext", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getContentResolver", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getDefault", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getInstance", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getResources", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getAssets", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "getSystemService", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "get", METHOD_TYPE_OBJECT},
    {MID_OBJECT, "randomUUID", METHOD_TYPE_OBJECT},
    {MID_EMPTY, "getString", METHOD_TYPE_OBJECT},
    {MID_DEVICE_ID, "toString", METHOD_TYPE_OBJECT},
    {MID_EMPTY_BYTES, "getByteArray", METHOD_TYPE_OBJECT},
    {MID_RESOURCE_PATH, "getSDFolder", METHOD_TYPE_OBJECT},
    {MID_RESOURCE_PATH, "nativeGetSdFolderPath", METHOD_TYPE_OBJECT},
    {MID_RESOURCE_PATH, "getExternalStorageDirectory", METHOD_TYPE_OBJECT},
    {MID_FILES_PATH, "getSaveFolder", METHOD_TYPE_OBJECT},
    {MID_FILES_PATH, "getFilesDir", METHOD_TYPE_OBJECT},
    {MID_FILES_PATH, "getAbsolutePath", METHOD_TYPE_OBJECT},
    {MID_FILES_PATH, "getPath", METHOD_TYPE_OBJECT},
    {MID_CACHE_PATH, "getCacheDir", METHOD_TYPE_OBJECT},
    {MID_MAIN_OBB, "getObbFileName", METHOD_TYPE_OBJECT},
    {MID_PATCH_OBB, "getObbPatchFileName", METHOD_TYPE_OBJECT},
    {MID_PACKAGE, "getPackageName", METHOD_TYPE_OBJECT},
    {MID_PACKAGE, "getPackage", METHOD_TYPE_OBJECT},
    {MID_COUNTRY, "getLocaleCountry", METHOD_TYPE_OBJECT},
    {MID_COUNTRY, "getCountry", METHOD_TYPE_OBJECT},
    {MID_ISO3_COUNTRY, "getISO3Country", METHOD_TYPE_OBJECT},
    {MID_LANGUAGE, "getLanguage", METHOD_TYPE_OBJECT},
    {MID_ISO3_LANGUAGE, "getISO3Language", METHOD_TYPE_OBJECT},
    {MID_DEVICE_ID, "getDeviceId", METHOD_TYPE_OBJECT},
    {MID_DEVICE_ID, "getHDIDFV", METHOD_TYPE_OBJECT},
    {MID_DEVICE_ID, "getIdentifier", METHOD_TYPE_OBJECT},
    {MID_DEVICE_ID, "getIMEI", METHOD_TYPE_OBJECT},
    {MID_PHONE_MODEL, "getPhoneModel", METHOD_TYPE_OBJECT},
    {MID_PHONE_MANUFACTURER, "getPhoneManufacturer", METHOD_TYPE_OBJECT},
    {MID_PHONE_FIRMWARE, "getPhoneFW", METHOD_TYPE_OBJECT},
    {MID_GAME_VERSION, "getVersion", METHOD_TYPE_OBJECT},
    {MID_MAC, "getMac", METHOD_TYPE_OBJECT},
    {MID_USER_AGENT, "getUserAgent", METHOD_TYPE_OBJECT},
    {MID_EMPTY, "getGLiveUsername", METHOD_TYPE_OBJECT},
    {MID_EMPTY, "getGLiveUserpassword", METHOD_TYPE_OBJECT},
    {MID_EMPTY, "getPreferenceString", METHOD_TYPE_OBJECT},
    {MID_SHARED_VALUE, "getSharedValue", METHOD_TYPE_OBJECT},
    {MID_RESOURCE_BYTES, "getResource", METHOD_TYPE_OBJECT},

    {MID_FALSE, "isUserMusicPlaying", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "isConnected", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "isNetworkConnected", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "isWifiEnabled", METHOD_TYPE_BOOLEAN},
    {MID_TRUE, "exists", METHOD_TYPE_BOOLEAN},
    {MID_TRUE, "mkdirs", METHOD_TYPE_BOOLEAN},
    {MID_SET_CURRENT_CONTEXT, "setCurrentContext", METHOD_TYPE_BOOLEAN},
    {MID_TRUE, "CheckDoneAcceptPP", METHOD_TYPE_BOOLEAN},
    {MID_TRUE, "isAndroidOrientationAvailable", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "IsFirstLaunched", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "isZEUSDevice", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "isSlideEnabled", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "getBoolean", METHOD_TYPE_BOOLEAN},
    {MID_FALSE, "containsKey", METHOD_TYPE_BOOLEAN},
    {MID_SHARED_HAS, "isSharedValue", METHOD_TYPE_BOOLEAN},

    {MID_ZERO, "getPhoneLanguage", METHOD_TYPE_INT},
    {MID_ZERO, "getLanguageId", METHOD_TYPE_INT},
    {MID_WIDTH, "getWidth", METHOD_TYPE_INT},
    {MID_WIDTH, "getScreenWidth", METHOD_TYPE_INT},
    {MID_HEIGHT, "getHeight", METHOD_TYPE_INT},
    {MID_HEIGHT, "getScreenHeight", METHOD_TYPE_INT},
    {MID_ZERO, "isWifiAlive", METHOD_TYPE_INT},
    {MID_TRUE, "IsPowerAConnected", METHOD_TYPE_INT},
    {MID_ZERO, "getInt", METHOD_TYPE_INT},
    {MID_AUDIO_SAMPLE_RATE, "getNativeOutputSampleRate", METHOD_TYPE_INT},
    {MID_AUDIO_BUFFER_SIZE, "getMinBufferSize", METHOD_TYPE_INT},
    {MID_AUDIO_PLAY_STATE, "getPlayState", METHOD_TYPE_INT},
    {MID_AUDIO_WRITE, "write", METHOD_TYPE_INT},
    {MID_MEMORY_INFO, "GetMemoryInfo", METHOD_TYPE_FLOAT},
    {MID_CURRENT_TIME, "currentTimeMillis", METHOD_TYPE_LONG},
    {MID_CURRENT_TIME, "GetCurrentTime", METHOD_TYPE_LONG},
    {MID_LONG_ZERO, "getLong", METHOD_TYPE_LONG},

    {MID_VIEW_SETTINGS, "setViewSettings", METHOD_TYPE_VOID},
    {MID_VOID, "startGame", METHOD_TYPE_VOID},
    {MID_VOID, "loadSound", METHOD_TYPE_VOID},
    {MID_VOID, "playSound", METHOD_TYPE_VOID},
    {MID_VOID, "pauseUserMusic", METHOD_TYPE_VOID},
    {MID_VOID, "stopUserMusic", METHOD_TYPE_VOID},
    {MID_VOID, "vibrate", METHOD_TYPE_VOID},
    {MID_VOID, "runOnUiThread", METHOD_TYPE_VOID},
    {MID_VOID, "sendAppToBackground", METHOD_TYPE_VOID},
    {MID_VOID, "openBrowser", METHOD_TYPE_VOID},
    {MID_VOID, "Exit", METHOD_TYPE_VOID},
    {MID_SETUP_PATHS, "setupPaths", METHOD_TYPE_VOID},
    {MID_VOID, "createView", METHOD_TYPE_VOID},
    {MID_VOID, "enableAccelerometer", METHOD_TYPE_VOID},
    {MID_VOID, "SetDisableKey", METHOD_TYPE_VOID},
    {MID_VOID, "InGameBrowser_SetBaseUrl", METHOD_TYPE_VOID},
    {MID_VOID, "InGameBrowser_OpenUrl", METHOD_TYPE_VOID},
    {MID_VOID, "SetDoneAcceptPP", METHOD_TYPE_VOID},
    {MID_VOID, "RateApp", METHOD_TYPE_VOID},
    {MID_VOID, "GetGangster4ButtonSwitch", METHOD_TYPE_VOID},
    {MID_VOID, "LaunchFacebook", METHOD_TYPE_VOID},
    {MID_VOID, "LaunchTwitter", METHOD_TYPE_VOID},
    {MID_VOID, "NoBackWarning", METHOD_TYPE_VOID},
    {MID_VOID, "setPSKeyEnable", METHOD_TYPE_VOID},
    {MID_VOID, "launchGLLive", METHOD_TYPE_VOID},
    {MID_VOID, "nativeGetLangIngame", METHOD_TYPE_VOID},
    {MID_VOID, "notifyTrophy", METHOD_TYPE_VOID},
    {MID_VOID, "openIGP", METHOD_TYPE_VOID},
    {MID_VOID, "setClearFrames", METHOD_TYPE_VOID},
    {MID_VOID, "setNoSwapFrames", METHOD_TYPE_VOID},
    {MID_VOID, "launchG4Button", METHOD_TYPE_VOID},
    {MID_VOID, "launchCustomerCare", METHOD_TYPE_VOID},
    {MID_VOID, "launchWelcomeScr", METHOD_TYPE_VOID},
    {MID_VOID, "sendGameTracking", METHOD_TYPE_VOID},
    {MID_VOID, "clear", METHOD_TYPE_VOID},
    {MID_VOID, "CloseGame", METHOD_TYPE_VOID},
    {MID_SHARED_DELETE, "deleteSharedValue", METHOD_TYPE_VOID},
    {MID_VOID, "minimize", METHOD_TYPE_VOID},
    {MID_AUDIO_PAUSE, "pause", METHOD_TYPE_VOID},
    {MID_AUDIO_PLAY, "play", METHOD_TYPE_VOID},
    {MID_VOID, "putBoolean", METHOD_TYPE_VOID},
    {MID_VOID, "putByteArray", METHOD_TYPE_VOID},
    {MID_VOID, "putInt", METHOD_TYPE_VOID},
    {MID_VOID, "putLong", METHOD_TYPE_VOID},
    {MID_VOID, "putString", METHOD_TYPE_VOID},
    {MID_AUDIO_RELEASE, "release", METHOD_TYPE_VOID},
    {MID_VOID, "SetAndroidOrientation", METHOD_TYPE_VOID},
    {MID_VOID, "SetAutoOrientationOff", METHOD_TYPE_VOID},
    {MID_VOID, "SetAutoOrientationOn", METHOD_TYPE_VOID},
    {MID_VOID, "setDisableKey", METHOD_TYPE_VOID},
    {MID_VOID, "setProcessPositionEvents", METHOD_TYPE_VOID},
    {MID_SHARED_SET, "setSharedValue", METHOD_TYPE_VOID},
    {MID_VOID, "setThreadPriority", METHOD_TYPE_VOID},
    {MID_AUDIO_STOP, "stop", METHOD_TYPE_VOID},
    {MID_VOID, "UpdatePreferences", METHOD_TYPE_VOID},
    {MID_VOID, "UpdatePreferences2", METHOD_TYPE_VOID},
};

MethodsBoolean methodsBoolean[] = {
    {MID_FALSE, boolean_false},
    {MID_TRUE, boolean_true},
    {MID_SET_CURRENT_CONTEXT, boolean_set_current_context},
    {MID_SHARED_HAS, boolean_shared_value},
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    {MID_MEMORY_INFO, float_memory_megabytes},
};
MethodsInt methodsInt[] = {
    {MID_ZERO, integer_zero},
    {MID_TRUE, integer_true},
    {MID_WIDTH, integer_width},
    {MID_HEIGHT, integer_height},
    {MID_AUDIO_SAMPLE_RATE, integer_audio_sample_rate},
    {MID_AUDIO_BUFFER_SIZE, integer_audio_buffer_size},
    {MID_AUDIO_PLAY_STATE, integer_audio_play_state},
    {MID_AUDIO_WRITE, integer_audio_write},
};
MethodsLong methodsLong[] = {
    {MID_CURRENT_TIME, current_time_millis},
    {MID_LONG_ZERO, long_zero},
};
MethodsObject methodsObject[] = {
    {MID_OBJECT, object_dummy},
    {MID_AUDIO_INIT, object_audio_track},
    {MID_EMPTY, string_empty},
    {MID_RESOURCE_PATH, string_resource_path},
    {MID_FILES_PATH, string_files_path},
    {MID_CACHE_PATH, string_cache_path},
    {MID_MAIN_OBB, string_main_obb},
    {MID_PATCH_OBB, string_patch_obb},
    {MID_PACKAGE, string_package},
    {MID_COUNTRY, string_country},
    {MID_LANGUAGE, string_language},
    {MID_DEVICE_ID, string_device_id},
    {MID_PHONE_MODEL, string_phone_model},
    {MID_RESOURCE_BYTES, byte_array_missing},
    {MID_PHONE_MANUFACTURER, string_phone_manufacturer},
    {MID_PHONE_FIRMWARE, string_phone_firmware},
    {MID_GAME_VERSION, string_game_version},
    {MID_ISO3_COUNTRY, string_iso3_country},
    {MID_ISO3_LANGUAGE, string_iso3_language},
    {MID_MAC, string_mac},
    {MID_USER_AGENT, string_user_agent},
    {MID_EMPTY_BYTES, byte_array_empty},
    {MID_SHARED_VALUE, string_shared_value},
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    {MID_VOID, method_void_stub},
    {MID_SETUP_PATHS, method_setup_paths},
    {MID_VIEW_SETTINGS, method_set_view_settings},
    {MID_SHARED_SET, method_set_shared_value},
    {MID_SHARED_DELETE, method_delete_shared_value},
    {MID_AUDIO_PLAY, method_audio_play},
    {MID_AUDIO_PAUSE, method_audio_pause},
    {MID_AUDIO_STOP, method_audio_stop},
    {MID_AUDIO_RELEASE, method_audio_release},
};

static const jint sdk_int = 19;
static char model[] = "PlayStation Vita";
static char manufacturer[] = "Sony";
static char device[] = "PS Vita";
static char window_service[] = "window";

NameToFieldID nameToFieldId[] = {
    {1, "SDK_INT", FIELD_TYPE_INT},
    {2, "MODEL", FIELD_TYPE_OBJECT},
    {3, "MANUFACTURER", FIELD_TYPE_OBJECT},
    {4, "DEVICE", FIELD_TYPE_OBJECT},
    {5, "PRODUCT", FIELD_TYPE_OBJECT},
    {6, "WINDOW_SERVICE", FIELD_TYPE_OBJECT},
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    {1, sdk_int},
};
FieldsObject fieldsObject[] = {
    {2, (jobject)model},
    {3, (jobject)manufacturer},
    {4, (jobject)device},
    {5, (jobject)device},
    {6, (jobject)window_service},
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

void java_init(void) {
    graphics_context_owner = sceKernelGetThreadId();
    LOG_INFO("GL CONTEXT: single owner=0x%x", graphics_context_owner);
    fieldsObject[0].value = string_value(model);
    fieldsObject[1].value = string_value(manufacturer);
    fieldsObject[2].value = string_value(device);
    fieldsObject[3].value = string_value(device);
    fieldsObject[4].value = string_value(window_service);
}

void java_configure_set_paths(RioSetPathsCall callback, jclass clazz) {
    set_paths_callback = callback;
    set_paths_class = clazz;
    paths_ready = 0;
}

int java_paths_ready(void) {
    return paths_ready;
}

__FALSOJNI_IMPL_CONTAINER_SIZES
