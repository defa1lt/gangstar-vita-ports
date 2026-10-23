#include "FalsoJNI.h"
#include <pthread.h>
#include <string.h>

extern void port_trace(const char *format, ...);

static int string_is(JNIEnv *env, jstring value, const char *expected) {
    const char *chars = (*env)->GetStringUTFChars(env, value, NULL);
    int result = chars && !strcmp(chars, expected);
    if (chars) (*env)->ReleaseStringUTFChars(env, value, chars);
    return result;
}

static void *reference_worker(void *shared) {
    JNIEnv *env = NULL;
    if (jvm->AttachCurrentThread(&jvm, &env, NULL) != JNI_OK) return (void *)1;
    jobject local = (*env)->NewLocalRef(env, shared);
    int passed = local && string_is(env, local, "shared");
    (*env)->NewStringUTF(env, "released on detach");
    jvm->DetachCurrentThread(&jvm);
    return passed ? NULL : (void *)1;
}

int fjni_run_reference_tests(JNIEnv *env) {
    unsigned created_before, destroyed_before, created, destroyed;
    fjni_reference_stats(&created_before, &destroyed_before);
    int passed = (*env)->PushLocalFrame(env, 16) == JNI_OK;
    jclass clazz = (*env)->FindClass(env, "java/lang/String");
    passed &= clazz != NULL;
    for (unsigned i = 0; i < 10000; ++i) {
        jclass again = (*env)->FindClass(env, "java/lang/String");
        passed &= again == clazz;
        (*env)->DeleteLocalRef(env, again);
    }
    (*env)->DeleteGlobalRef(env, (*env)->NewGlobalRef(env, clazz));
    passed &= (*env)->FindClass(env, "java/lang/String") == clazz;

    jstring string = (*env)->NewStringUTF(env, "global survives local deletion");
    jobject global = (*env)->NewGlobalRef(env, string);
    (*env)->DeleteLocalRef(env, string);
    passed &= global && string_is(env, global, "global survives local deletion");
    string = (*env)->NewLocalRef(env, global);
    (*env)->DeleteGlobalRef(env, global);
    passed &= string && string_is(env, string, "global survives local deletion");
    (*env)->DeleteLocalRef(env, string);

    (*env)->PushLocalFrame(env, 4);
    jbyteArray bytes = (*env)->NewByteArray(env, 128);
    const jbyte expected[] = {3, 9, 27};
    (*env)->SetByteArrayRegion(env, bytes, 10, 3, expected);
    global = (*env)->NewGlobalRef(env, bytes);
    (*env)->PopLocalFrame(env, NULL);
    jbyte actual[3] = {0};
    (*env)->GetByteArrayRegion(env, global, 10, 3, actual);
    passed &= !memcmp(actual, expected, sizeof(actual));
    (*env)->DeleteGlobalRef(env, global);

    (*env)->PushLocalFrame(env, 4);
    string = (*env)->NewStringUTF(env, "promoted");
    string = (*env)->PopLocalFrame(env, string);
    passed &= string && string_is(env, string, "promoted");
    (*env)->DeleteLocalRef(env, string);

    string = (*env)->NewStringUTF(env, "array keeps element");
    jobjectArray array = (*env)->NewObjectArray(env, 1, clazz, string);
    (*env)->DeleteLocalRef(env, string);
    jstring replacement = (*env)->NewStringUTF(env, "replacement");
    (*env)->SetObjectArrayElement(env, array, 0, replacement);
    (*env)->DeleteLocalRef(env, replacement);
    string = (*env)->GetObjectArrayElement(env, array, 0);
    (*env)->DeleteLocalRef(env, array);
    passed &= string && string_is(env, string, "replacement");
    (*env)->DeleteLocalRef(env, string);

    string = (*env)->NewStringUTF(env, "shared");
    global = (*env)->NewGlobalRef(env, string);
    (*env)->DeleteLocalRef(env, string);
    pthread_t worker;
    void *worker_result = (void *)1;
    int started = pthread_create(&worker, NULL, reference_worker, global) == 0;
    passed &= started;
    if (started) {
        passed &= pthread_join(worker, &worker_result) == 0;
        passed &= worker_result == NULL;
    }
    (*env)->DeleteGlobalRef(env, global);

    static int borrowed;
    (*env)->DeleteGlobalRef(env, (*env)->NewGlobalRef(env, &borrowed));
    for (unsigned i = 0; i < 1000; ++i) {
        (*env)->PushLocalFrame(env, 4);
        passed &= (*env)->NewStringUTF(env, "temporary") != NULL;
        passed &= (*env)->NewByteArray(env, 64) != NULL;
        (*env)->PopLocalFrame(env, NULL);
    }
    (*env)->PopLocalFrame(env, NULL);
    fjni_reference_stats(&created, &destroyed);
    passed &= created - created_before == destroyed - destroyed_before;
    port_trace("JNI REFS TEST: %s created=%u destroyed=%u live=%u classes=10000 frames=1000 thread_detach=1",
               passed ? "PASS" : "FAIL", created - created_before,
               destroyed - destroyed_before, created - destroyed);
    return passed ? 0 : -1;
}
