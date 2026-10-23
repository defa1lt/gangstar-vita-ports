#pragma once
#include <stdarg.h>
#include <falso_jni/FalsoJNI_Impl.h>

enum {
    AUDIO_CREATE=1000, AUDIO_RATE, AUDIO_BUFFER, AUDIO_WRITE, AUDIO_STATE,
    AUDIO_PLAY, AUDIO_PAUSE, AUDIO_STOP, AUDIO_RELEASE,
};
jobject vegas_audio_create(jmethodID id, va_list arguments);
jint vegas_audio_integer(jmethodID id, va_list arguments);
void vegas_audio_void(jmethodID id, va_list arguments);
