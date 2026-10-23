#include "audio.h"
#include "diagnostics.h"
#include <psp2/audioout.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { GRAIN=1024, FRAME_BYTES=4, JDA_MAGIC=0xB00B1E5 };
static pthread_mutex_t audio_lock=PTHREAD_MUTEX_INITIALIZER;
static int port=-1, play_state=1, accumulated;
static int16_t pcm[GRAIN*2];
static unsigned outputs;
static int signal_reported;

jobject vegas_audio_create(jmethodID id, va_list arguments) {
    (void)id;
    int stream=va_arg(arguments,int), rate=va_arg(arguments,int);
    int channels=va_arg(arguments,int), format=va_arg(arguments,int);
    int buffer=va_arg(arguments,int), mode=va_arg(arguments,int);
    if (stream!=3 || rate!=44100 || channels!=12 || format!=2 ||
        buffer<GRAIN*FRAME_BYTES || buffer%FRAME_BYTES || mode!=1) {
        LOG_ERROR("Unsupported AudioTrack configuration: %d/%d/%d/%d/%d/%d",
                  stream,rate,channels,format,buffer,mode);
        return NULL;
    }
    uint32_t *object=malloc(sizeof(*object));
    if (!object) return NULL;
    *object=0x4154524b;
    pthread_mutex_lock(&audio_lock);
    if (port>=0) {
        pthread_mutex_unlock(&audio_lock);
        free(object);
        LOG_ERROR("A second AudioTrack was requested before release");
        return NULL;
    }
    int opened=sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,GRAIN,rate,SCE_AUDIO_OUT_MODE_STEREO);
    if (opened>=0) {
        port=opened; play_state=1; accumulated=0; outputs=0; signal_reported=0;
    }
    pthread_mutex_unlock(&audio_lock);
    if (opened<0) { free(object); LOG_ERROR("AudioTrack port open failed: 0x%x",opened); return NULL; }
    // FalsoJNI owns this heap token after NewGlobalRef / DeleteGlobalRef.
    LOG_SUCCESS("AudioTrack opened: port=%d 44100 Hz stereo PCM16 grain=%d",opened,GRAIN);
    return (jobject)object;
}

static int write_pcm(JavaDynArray *array, int offset, int size) {
    if (!array || array->magic!=JDA_MAGIC || array->type!=FIELD_TYPE_BYTE ||
        !array->array || array->len<0 || offset<0 || size<0 || offset>array->len ||
        size>array->len-offset || offset%FRAME_BYTES || size%FRAME_BYTES) return -2;
    pthread_mutex_lock(&audio_lock);
    if (port<0) { pthread_mutex_unlock(&audio_lock); return -3; }
    if (play_state!=3) { pthread_mutex_unlock(&audio_lock); return 0; }
    const unsigned char *samples=(const unsigned char *)array->array+offset;
    int accepted=0;
    while (accepted<size) {
        int before=accumulated;
        int take=GRAIN-accumulated;
        if (take>(size-accepted)/FRAME_BYTES) take=(size-accepted)/FRAME_BYTES;
        memcpy(pcm+accumulated*2,samples+accepted,(size_t)take*FRAME_BYTES);
        accumulated+=take;
        if (accumulated==GRAIN) {
            int result=sceAudioOutOutput(port,pcm);
            if (result<0) {
                accumulated=before;
                pthread_mutex_unlock(&audio_lock);
                LOG_ERROR("AudioTrack output failed: 0x%x",result);
                return accepted ? accepted : -3;
            }
            accumulated=0;
            ++outputs;
            if (!signal_reported) {
                for (unsigned i=0;i<GRAIN*2;++i) if (pcm[i]) {
                    signal_reported=1;
                    LOG_SUCCESS("AudioTrack emitted nonzero PCM: output=%u",outputs);
                    break;
                }
            }
        }
        accepted+=take*FRAME_BYTES;
    }
    pthread_mutex_unlock(&audio_lock);
    return accepted;
}

jint vegas_audio_integer(jmethodID id, va_list arguments) {
    switch ((int)id) {
        case AUDIO_RATE:
            (void)va_arg(arguments,int);
            return 44100;
        case AUDIO_BUFFER: {
            int rate=va_arg(arguments,int), channels=va_arg(arguments,int), format=va_arg(arguments,int);
            return rate==44100 && channels==12 && format==2 ? GRAIN*FRAME_BYTES : -2;
        }
        case AUDIO_WRITE: {
            JavaDynArray *array=va_arg(arguments,JavaDynArray *);
            int offset=va_arg(arguments,int), size=va_arg(arguments,int);
            return write_pcm(array,offset,size);
        }
        case AUDIO_STATE: {
            pthread_mutex_lock(&audio_lock);
            int state=play_state;
            pthread_mutex_unlock(&audio_lock);
            return state;
        }
    }
    return -3;
}

void vegas_audio_void(jmethodID id, va_list arguments) {
    (void)arguments;
    pthread_mutex_lock(&audio_lock);
    if (port>=0) {
        switch ((int)id) {
            case AUDIO_PLAY: play_state=3; break;
            case AUDIO_PAUSE: play_state=2; break;
            case AUDIO_STOP: play_state=1; accumulated=0; break;
            case AUDIO_RELEASE:
                sceAudioOutOutput(port,NULL);
                sceAudioOutReleasePort(port);
                port=-1; play_state=1; accumulated=0;
                break;
        }
    }
    pthread_mutex_unlock(&audio_lock);
}
