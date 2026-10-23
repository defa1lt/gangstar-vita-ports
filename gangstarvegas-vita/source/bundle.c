#include "bundle.h"
#include "diagnostics.h"
#include <falso_jni/FalsoJNI_ImplBridge.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

#define PREFERENCE_FILE "ux0:data/gangstarvegas/files/preferences.bin"
#define VALUE_LIMIT (1024u * 1024u)
#define FILE_LIMIT (16u * 1024u * 1024u)

enum ValueType { VALUE_INT, VALUE_LONG, VALUE_BOOLEAN, VALUE_STRING, VALUE_BYTES };
typedef struct Value {
    enum ValueType type;
    jlong number;
    void *data;
    uint32_t size;
    int is_null;
} Value;
typedef struct Entry {
    char *key;
    Value value;
    struct Entry *next;
} Entry;
typedef struct Bundle {
    Entry *entries;
} Bundle;
enum ObjectType { OBJECT_BUNDLE, OBJECT_STRING, OBJECT_BYTES };
typedef struct Reference {
    jobject object;
    enum ObjectType type;
    unsigned locals, globals;
    struct Reference *next;
} Reference;
typedef struct Preference {
    char *name, *key;
    Value value;
    struct Preference *next;
} Preference;

static struct JNINativeInterface saved;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static Reference *references;
static Preference *preferences;
static int preferences_loaded;
static int installed;

enum Method {
    CONSTRUCTOR, PUT_STRING, GET_STRING, PUT_INT, GET_INT, PUT_LONG, GET_LONG,
    PUT_BOOLEAN, GET_BOOLEAN, PUT_BYTES, GET_BYTES, CONTAINS_KEY, CLEAR, METHOD_COUNT
};
static const struct {
    const char *name, *signature;
} methods[METHOD_COUNT] = {
    {"<init>", "()V"},
    {"putString", "(Ljava/lang/String;Ljava/lang/String;)V"},
    {"getString", "(Ljava/lang/String;)Ljava/lang/String;"},
    {"putInt", "(Ljava/lang/String;I)V"},
    {"getInt", "(Ljava/lang/String;)I"},
    {"putLong", "(Ljava/lang/String;J)V"},
    {"getLong", "(Ljava/lang/String;)J"},
    {"putBoolean", "(Ljava/lang/String;Z)V"},
    {"getBoolean", "(Ljava/lang/String;)Z"},
    {"putByteArray", "(Ljava/lang/String;[B)V"},
    {"getByteArray", "(Ljava/lang/String;)[B"},
    {"containsKey", "(Ljava/lang/String;)Z"},
    {"clear", "()V"}
};

static int method_index(jmethodID id) {
    for (int i = 0; i < METHOD_COUNT; ++i)
        if (id == (jmethodID)&methods[i]) return i;
    return -1;
}
static void value_clear(Value *value) {
    free(value->data);
    memset(value, 0, sizeof(*value));
}
static int value_copy(Value *target, const Value *source) {
    *target = *source;
    target->data = NULL;
    if (source->data) {
        size_t size = source->size + (source->type == VALUE_STRING);
        target->data = malloc(size ? size : 1);
        if (!target->data) return -1;
        memcpy(target->data, source->data, size);
    }
    return 0;
}
static char *string_copy(jstring string) {
    if (!string) return NULL;
    const char *bytes = saved.GetStringUTFChars(&jni, string, NULL);
    if (!bytes) return NULL;
    size_t size = strlen(bytes);
    char *copy = size <= VALUE_LIMIT ? strdup(bytes) : NULL;
    saved.ReleaseStringUTFChars(&jni, string, (char *)bytes);
    return copy;
}
static Reference *reference_find(jobject object) {
    for (Reference *ref = references; ref; ref = ref->next)
        if (ref->object == object) return ref;
    return NULL;
}
static Bundle *bundle_find(jobject object) {
    Reference *ref = reference_find(object);
    return ref && ref->type == OBJECT_BUNDLE ? (Bundle *)object : NULL;
}
static int reference_add(jobject object, enum ObjectType type) {
    if (!object) return -1;
    Reference *ref = calloc(1, sizeof(*ref));
    if (!ref) return -1;
    ref->object = object;
    ref->type = type;
    ref->locals = 1;
    ref->next = references;
    references = ref;
    return 0;
}
static void entries_clear(Bundle *bundle) {
    while (bundle->entries) {
        Entry *entry = bundle->entries;
        bundle->entries = entry->next;
        free(entry->key);
        value_clear(&entry->value);
        free(entry);
    }
}
static void object_destroy(Reference *ref) {
    if (ref->type == OBJECT_BUNDLE) {
        entries_clear(ref->object);
        free(ref->object);
    } else if (ref->type == OBJECT_STRING) {
        JavaString *string = ref->object;
        jda_free(string->utf8);
        jda_free(string->utf16);
        free(string);
    } else {
        saved.DeleteGlobalRef(&jni, ref->object);
    }
    free(ref);
}
static Entry *entry_find(Bundle *bundle, const char *key) {
    for (Entry *entry = bundle->entries; entry; entry = entry->next)
        if (!strcmp(entry->key, key)) return entry;
    return NULL;
}
static int entry_set(Bundle *bundle, const char *key, const Value *value) {
    Value copy;
    if (value_copy(&copy, value)) return -1;
    Entry *entry = entry_find(bundle, key);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) { value_clear(&copy); return -1; }
        entry->key = strdup(key);
        if (!entry->key) { value_clear(&copy); free(entry); return -1; }
        entry->next = bundle->entries;
        bundle->entries = entry;
    }
    value_clear(&entry->value);
    entry->value = copy;
    return 0;
}
static jobject new_bundle(void) {
    Bundle *bundle = calloc(1, sizeof(*bundle));
    if (bundle && reference_add(bundle, OBJECT_BUNDLE)) {
        free(bundle);
        bundle = NULL;
    }
    return bundle;
}
static jobject value_object(const Value *value) {
    if (!value || value->is_null) return NULL;
    jobject object = NULL;
    enum ObjectType type;
    if (value->type == VALUE_STRING) {
        object = saved.NewStringUTF(&jni, value->data ? value->data : "");
        type = OBJECT_STRING;
    } else if (value->type == VALUE_BYTES) {
        object = saved.NewByteArray(&jni, value->size);
        if (object && value->size)
            saved.SetByteArrayRegion(&jni, object, 0, value->size, value->data);
        type = OBJECT_BYTES;
    } else return NULL;
    if (object && reference_add(object, type)) {
        if (type == OBJECT_STRING) {
            JavaString *string = object;
            jda_free(string->utf8); jda_free(string->utf16); free(string);
        } else saved.DeleteGlobalRef(&jni, object);
        return NULL;
    }
    return object;
}
static jmethodID get_method(JNIEnv *env, jclass clazz, const char *name, const char *signature) {
    if (clazz && (uintptr_t)clazz != 0x42424242 &&
        !strcmp((const char *)clazz, "android/os/Bundle")) {
        for (int i = 0; i < METHOD_COUNT; ++i)
            if (!strcmp(name, methods[i].name) && !strcmp(signature, methods[i].signature))
                return (jmethodID)&methods[i];
        LOG_ERROR("Unsupported Bundle method: %s %s", name, signature);
        return NULL;
    }
    return saved.GetMethodID(env, clazz, name, signature);
}
static jclass object_class(JNIEnv *env, jobject object) {
    pthread_mutex_lock(&mutex);
    int is_bundle = bundle_find(object) != NULL;
    pthread_mutex_unlock(&mutex);
    return is_bundle ? saved.FindClass(env, "android/os/Bundle") : saved.GetObjectClass(env, object);
}
static jobject new_object_v(JNIEnv *env, jclass clazz, jmethodID method, va_list args) {
    if (method_index(method) != CONSTRUCTOR) return saved.NewObjectV(env, clazz, method, args);
    pthread_mutex_lock(&mutex);
    jobject object = new_bundle();
    pthread_mutex_unlock(&mutex);
    return object;
}
static jobject new_object(JNIEnv *env, jclass clazz, jmethodID method, ...) {
    va_list args; va_start(args, method);
    jobject object = new_object_v(env, clazz, method, args);
    va_end(args); return object;
}
static jobject new_object_a(JNIEnv *env, jclass clazz, jmethodID method, const jvalue *args) {
    if (method_index(method) != CONSTRUCTOR) return saved.NewObjectA(env, clazz, method, args);
    pthread_mutex_lock(&mutex);
    jobject object = new_bundle();
    pthread_mutex_unlock(&mutex);
    return object;
}

static jvalue invoke(jobject object, int method, const jvalue *args) {
    jvalue result = {.j = 0};
    pthread_mutex_lock(&mutex);
    Bundle *bundle = bundle_find(object);
    if (!bundle) { LOG_ERROR("Bundle call has invalid receiver"); goto done; }
    if (method == CLEAR) { entries_clear(bundle); goto done; }
    char *key = string_copy(args[0].l);
    if (!key) goto done;
    Entry *entry = entry_find(bundle, key);
    Value value = {0};
    switch (method) {
        case PUT_STRING:
            value.type = VALUE_STRING;
            value.is_null = args[1].l == NULL;
            value.data = string_copy(args[1].l);
            if (!value.is_null && !value.data) break;
            value.size = value.data ? strlen(value.data) : 0;
            if (entry_set(bundle, key, &value)) LOG_ERROR("Bundle string allocation failed");
            value_clear(&value);
            break;
        case PUT_BYTES:
            value.type = VALUE_BYTES;
            value.is_null = args[1].l == NULL;
            if (!value.is_null) {
                jsize size = saved.GetArrayLength(&jni, args[1].l);
                if (size < 0 || (unsigned)size > VALUE_LIMIT) break;
                value.size = (uint32_t)size;
                value.data = malloc(size ? (size_t)size : 1);
                if (!value.data) break;
                saved.GetByteArrayRegion(&jni, args[1].l, 0, size, value.data);
            }
            if (entry_set(bundle, key, &value)) LOG_ERROR("Bundle byte array allocation failed");
            value_clear(&value);
            break;
        case PUT_INT: case PUT_LONG: case PUT_BOOLEAN:
            value.type = method == PUT_INT ? VALUE_INT : method == PUT_LONG ? VALUE_LONG : VALUE_BOOLEAN;
            value.number = method == PUT_LONG ? args[1].j : method == PUT_INT ? args[1].i : !!args[1].z;
            if (entry_set(bundle, key, &value)) LOG_ERROR("Bundle scalar allocation failed");
            break;
        case GET_STRING: case GET_BYTES:
            if (entry && entry->value.type == (method == GET_STRING ? VALUE_STRING : VALUE_BYTES))
                result.l = value_object(&entry->value);
            break;
        case GET_INT:
            if (entry && entry->value.type == VALUE_INT) result.i = (jint)entry->value.number;
            break;
        case GET_LONG:
            if (entry && entry->value.type == VALUE_LONG) result.j = entry->value.number;
            break;
        case GET_BOOLEAN:
            if (entry && entry->value.type == VALUE_BOOLEAN) result.z = !!entry->value.number;
            break;
        case CONTAINS_KEY: result.z = entry != NULL; break;
        default: break;
    }
    free(key);
done:
    pthread_mutex_unlock(&mutex);
    return result;
}
static void arguments_v(int method, va_list input, jvalue args[2]) {
    va_list copy; va_copy(copy, input);
    if (method != CLEAR) args[0].l = va_arg(copy, jobject);
    switch (method) {
        case PUT_STRING: case PUT_BYTES: args[1].l = va_arg(copy, jobject); break;
        case PUT_INT: args[1].i = va_arg(copy, jint); break;
        case PUT_BOOLEAN: args[1].z = va_arg(copy, int) != 0; break;
        case PUT_LONG: args[1].j = va_arg(copy, jlong); break;
        default: break;
    }
    va_end(copy);
}
#define WRAP_CALL(label, ctype, member) \
static ctype call_##label##_v(JNIEnv *env, jobject object, jmethodID method, va_list input) { \
    int index = method_index(method); \
    if (index < 0) return saved.Call##label##MethodV(env, object, method, input); \
    jvalue args[2] = {{0}, {0}}; arguments_v(index, input, args); \
    return invoke(object, index, args).member; \
} \
static ctype call_##label(JNIEnv *env, jobject object, jmethodID method, ...) { \
    va_list args; va_start(args, method); \
    ctype value = call_##label##_v(env, object, method, args); va_end(args); return value; \
} \
static ctype call_##label##_a(JNIEnv *env, jobject object, jmethodID method, const jvalue *args) { \
    int index = method_index(method); \
    if (index < 0) return saved.Call##label##MethodA(env, object, method, args); \
    return invoke(object, index, args).member; \
}
WRAP_CALL(Object, jobject, l)
WRAP_CALL(Int, jint, i)
WRAP_CALL(Long, jlong, j)
WRAP_CALL(Boolean, jboolean, z)
static void call_void_v(JNIEnv *env, jobject object, jmethodID method, va_list input) {
    int index = method_index(method);
    if (index < 0) { saved.CallVoidMethodV(env, object, method, input); return; }
    jvalue args[2] = {{0}, {0}}; arguments_v(index, input, args);
    (void)invoke(object, index, args);
}
static void call_void(JNIEnv *env, jobject object, jmethodID method, ...) {
    va_list args; va_start(args, method); call_void_v(env, object, method, args); va_end(args);
}
static void call_void_a(JNIEnv *env, jobject object, jmethodID method, const jvalue *args) {
    int index = method_index(method);
    if (index < 0) saved.CallVoidMethodA(env, object, method, args);
    else (void)invoke(object, index, args);
}
static jobject new_reference(JNIEnv *env, jobject object, int global) {
    pthread_mutex_lock(&mutex);
    Reference *ref = reference_find(object);
    if (ref) {
        if (global) ++ref->globals; else ++ref->locals;
    }
    pthread_mutex_unlock(&mutex);
    if (ref) return object;
    return global ? saved.NewGlobalRef(env, object) : saved.NewLocalRef(env, object);
}
static jobject new_global(JNIEnv *env, jobject object) { return new_reference(env, object, 1); }
static jobject new_local(JNIEnv *env, jobject object) { return new_reference(env, object, 0); }
static void delete_reference(JNIEnv *env, jobject object, int global) {
    pthread_mutex_lock(&mutex);
    Reference **link = &references;
    while (*link && (*link)->object != object) link = &(*link)->next;
    Reference *ref = *link;
    if (ref) {
        /* Native ABundle_New returns a local reference which its C++ callers
         * release with DeleteGlobalRef instead of promoting it first. */
        if (global && ref->globals) --ref->globals;
        else if (ref->locals) --ref->locals;
        if (!ref->locals && !ref->globals) { *link = ref->next; object_destroy(ref); }
    }
    pthread_mutex_unlock(&mutex);
    if (!ref) {
        if (global) saved.DeleteGlobalRef(env, object); else saved.DeleteLocalRef(env, object);
    }
}
static void delete_global(JNIEnv *env, jobject object) { delete_reference(env, object, 1); }
static void delete_local(JNIEnv *env, jobject object) { delete_reference(env, object, 0); }

static Preference *preference_find(const char *name, const char *key) {
    for (Preference *pref = preferences; pref; pref = pref->next)
        if (!strcmp(pref->name, name) && !strcmp(pref->key, key)) return pref;
    return NULL;
}
static int preference_assign(const char *name, const char *key, const Value *value) {
    Value copy;
    if (value_copy(&copy, value)) return -1;
    Preference *pref = preference_find(name, key);
    if (!pref) {
        pref = calloc(1, sizeof(*pref));
        if (!pref) { value_clear(&copy); return -1; }
        pref->name = strdup(name); pref->key = strdup(key);
        if (!pref->name || !pref->key) {
            free(pref->name); free(pref->key); free(pref); value_clear(&copy); return -1;
        }
        pref->next = preferences; preferences = pref;
    }
    value_clear(&pref->value); pref->value = copy;
    return 0;
}
static int read_exact(FILE *file, void *data, size_t size) {
    return !size || fread(data, 1, size, file) == size ? 0 : -1;
}
static void preferences_clear(void) {
    while (preferences) {
        Preference *pref = preferences;
        preferences = pref->next;
        free(pref->name); free(pref->key); value_clear(&pref->value); free(pref);
    }
}
static int regular_file(const char *path) {
    struct stat info;
    if (stat(path, &info)) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; return -1; }
    return 1;
}
static int rename_available(const char *source, const char *target) {
    int exists = regular_file(target);
    if (exists) { if (exists > 0) errno = EEXIST; return -1; }
#ifdef __vita__
    /* Vita libc rename unlinks its destination before calling sceIoRename. */
    int result = sceIoRename(source, target);
    if (result < 0) { LOG_ERROR("Preference rename failed: 0x%08x", result); errno = EIO; return -1; }
    return 0;
#else
    return rename(source, target);
#endif
}
static int preferences_read(const char *path) {
    preferences_clear();
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    char magic[8]; size_t total = 8;
    if (read_exact(file, magic, 8) || memcmp(magic, "GSVPREF1", 8)) goto invalid;
    for (;;) {
        uint32_t header[5];
        size_t count = fread(header, 1, sizeof(header), file);
        if (!count && feof(file)) break;
        if (count != sizeof(header) || header[0] > VALUE_STRING || header[1] > VALUE_LIMIT ||
            header[2] > VALUE_LIMIT || header[3] > VALUE_LIMIT || header[4] > 1) goto invalid;
        total += sizeof(header) + header[1] + header[2] + header[3];
        if (total > FILE_LIMIT) goto invalid;
        char *name = calloc((size_t)header[1] + 1, 1), *key = calloc((size_t)header[2] + 1, 1);
        Value value = {.type = header[0], .size = header[3], .is_null = header[4]};
        value.data = calloc((size_t)header[3] + 1, 1);
        if (!name || !key || !value.data || read_exact(file, name, header[1]) ||
            read_exact(file, key, header[2]) || read_exact(file, value.data, header[3])) {
            free(name); free(key); value_clear(&value); goto invalid;
        }
        int valid = !memchr(name, 0, header[1]) && !memchr(key, 0, header[2]);
        if (value.type == VALUE_STRING) valid = valid && !memchr(value.data, 0, value.size);
        else {
            valid = valid && value.size == sizeof(jlong) && !value.is_null;
            if (valid) memcpy(&value.number, value.data, sizeof(jlong));
            free(value.data); value.data = NULL; value.size = 0;
        }
        if (valid && preference_assign(name, key, &value)) valid = 0;
        free(name); free(key); value_clear(&value);
        if (!valid) goto invalid;
    }
    fclose(file); return 0;
invalid:
    preferences_clear(); fclose(file); return -1;
}
static void preferences_load(void) {
    if (preferences_loaded) return;
    preferences_loaded = 1;
    int current = regular_file(PREFERENCE_FILE);
    int previous = regular_file(PREFERENCE_FILE ".previous");
    if (current > 0 && !preferences_read(PREFERENCE_FILE)) {
        if (previous > 0 && unlink(PREFERENCE_FILE ".previous"))
            LOG_ERROR("Previous preference cleanup failed: %d", errno);
        return;
    }
    if (previous > 0 && !preferences_read(PREFERENCE_FILE ".previous")) {
        if (current == 0 && !rename_available(PREFERENCE_FILE ".previous", PREFERENCE_FILE)) {
            LOG_SUCCESS("Recovered previous preferences after interrupted save");
            return;
        }
        LOG_ERROR("Previous preferences recovered in memory; conflicting files preserved, writes disabled");
        preferences_loaded = -1; return;
    }
    if (!current && !previous) return;
    LOG_ERROR("Preferences file invalid or inaccessible; writes disabled to preserve existing data");
    preferences_loaded = -1;
}
static int matches(FILE *file, const void *data, size_t size) {
    unsigned char buffer[1024];
    const unsigned char *expected = data;
    while (size) {
        size_t count = size < sizeof(buffer) ? size : sizeof(buffer);
        if (read_exact(file, buffer, count) || memcmp(buffer, expected, count)) return -1;
        expected += count; size -= count;
    }
    return 0;
}
static int preferences_verify(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    int failed = matches(file, "GSVPREF1", 8);
    for (Preference *pref = preferences; pref && !failed; pref = pref->next) {
        uint32_t size = pref->value.type == VALUE_STRING ? pref->value.size : sizeof(jlong);
        uint32_t header[5] = {pref->value.type, strlen(pref->name), strlen(pref->key), size, pref->value.is_null};
        const void *data = pref->value.type == VALUE_STRING ? pref->value.data : &pref->value.number;
        failed = matches(file, header, sizeof(header)) || matches(file, pref->name, header[1]) ||
            matches(file, pref->key, header[2]) || matches(file, data, size);
    }
    if (!failed && (fgetc(file) != EOF || ferror(file))) failed = 1;
    fclose(file); return failed ? -1 : 0;
}
static int preferences_save(void) {
    if (preferences_loaded < 0) return -1;
    if (regular_file(PREFERENCE_FILE ".previous") != 0 || regular_file(PREFERENCE_FILE) < 0) {
        LOG_ERROR("Preferences save stopped: previous backup or invalid destination preserved");
        return -1;
    }
    FILE *file = fopen(PREFERENCE_FILE ".tmp", "wb");
    if (!file) { LOG_ERROR("Preferences write failed: %d", errno); return -1; }
    int failed = fwrite("GSVPREF1", 1, 8, file) != 8;
    size_t total = 8;
    for (Preference *pref = preferences; pref && !failed; pref = pref->next) {
        uint32_t size = pref->value.type == VALUE_STRING ? pref->value.size : sizeof(jlong);
        uint32_t header[5] = {pref->value.type, strlen(pref->name), strlen(pref->key), size, pref->value.is_null};
        total += sizeof(header) + header[1] + header[2] + size;
        const void *data = pref->value.type == VALUE_STRING ? pref->value.data : &pref->value.number;
        if (total > FILE_LIMIT || fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
            fwrite(pref->name, 1, header[1], file) != header[1] ||
            fwrite(pref->key, 1, header[2], file) != header[2] ||
            (size && fwrite(data, 1, size, file) != size)) failed = 1;
    }
    if (fflush(file) || fsync(fileno(file))) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || preferences_verify(PREFERENCE_FILE ".tmp")) {
        LOG_ERROR("Preferences temporary write verification failed: %d", errno); return -1;
    }
    int current = regular_file(PREFERENCE_FILE);
    if (current < 0 || (current && rename_available(PREFERENCE_FILE, PREFERENCE_FILE ".previous"))) return -1;
    if (rename_available(PREFERENCE_FILE ".tmp", PREFERENCE_FILE)) {
        if (current && rename_available(PREFERENCE_FILE ".previous", PREFERENCE_FILE))
            preferences_loaded = -1;
        LOG_ERROR("Preferences promotion failed; previous save preserved"); return -1;
    }
    if (preferences_verify(PREFERENCE_FILE)) {
        preferences_loaded = -1;
        LOG_ERROR("Preferences committed-file verification failed; previous save preserved"); return -1;
    }
    if (current && unlink(PREFERENCE_FILE ".previous"))
        LOG_ERROR("Preferences saved; previous backup cleanup failed: %d", errno);
    return 0;
}
static int preference_request(Bundle *bundle, const char **name, const char **key, int *type) {
    Entry *n = entry_find(bundle, "npPrefName"), *k = entry_find(bundle, "npKey");
    Entry *t = entry_find(bundle, "npDataType");
    if (!n || !k || !t || n->value.type != VALUE_STRING || k->value.type != VALUE_STRING ||
        n->value.is_null || k->value.is_null || t->value.type != VALUE_INT ||
        t->value.number < VALUE_INT || t->value.number > VALUE_STRING) return -1;
    *name = n->value.data; *key = k->value.data; *type = t->value.number;
    return 0;
}
jobject vegas_preference_get(jobject object) {
    pthread_mutex_lock(&mutex);
    Bundle *bundle = bundle_find(object);
    const char *name, *key; int type;
    if (!bundle || preference_request(bundle, &name, &key, &type)) {
        LOG_ERROR("nativeGetPreference: invalid Bundle request");
        pthread_mutex_unlock(&mutex); return object;
    }
    preferences_load();
    Preference *pref = preference_find(name, key);
    Entry *fallback = entry_find(bundle, "npDefaultValue");
    Value empty = {.type = type, .is_null = type == VALUE_STRING};
    const Value *value = pref && (int)pref->value.type == type ? &pref->value :
        fallback && (int)fallback->value.type == type ? &fallback->value : &empty;
    if (entry_set(bundle, "npResult", value)) LOG_ERROR("nativeGetPreference allocation failed");
    ++reference_find(object)->locals;
    pthread_mutex_unlock(&mutex);
    return object;
}
static void preference_store(const char *name, const char *key, const Value *value) {
    Preference *previous = preference_find(name, key);
    Value old_value = {0};
    if (preferences_loaded < 0 || (previous && value_copy(&old_value, &previous->value)) ||
        preference_assign(name, key, value)) LOG_ERROR("Preference write failed");
    else if (preferences_save()) {
        Preference *changed = preference_find(name, key);
        if (previous) {
            value_clear(&changed->value); changed->value = old_value;
            memset(&old_value, 0, sizeof(old_value));
        } else {
            Preference **link = &preferences;
            while (*link != changed) link = &(*link)->next;
            *link = changed->next;
            free(changed->name); free(changed->key); value_clear(&changed->value); free(changed);
        }
    }
    value_clear(&old_value);
}
jstring vegas_string_preference_get(const char *name, jstring key_string, jstring fallback_string) {
    char *key=string_copy(key_string);
    char *fallback=fallback_string?string_copy(fallback_string):strdup("");
    if (!fallback) { free(key); return NULL; }
    pthread_mutex_lock(&mutex);
    preferences_load();
    Preference *pref=key?preference_find(name,key):NULL;
    const char *value=pref && pref->value.type==VALUE_STRING && !pref->value.is_null ? pref->value.data : fallback;
    jstring result=saved.NewStringUTF(&jni,value);
    pthread_mutex_unlock(&mutex);
    free(key); free(fallback);
    return result;
}
void vegas_string_preference_set(const char *name, jstring key_string, jstring value_string) {
    char *key=string_copy(key_string);
    char *text=value_string?string_copy(value_string):strdup("");
    if (!key || !text) { free(key); free(text); LOG_ERROR("Shared value allocation failed"); return; }
    Value value={.type=VALUE_STRING,.data=text,.size=strlen(text)};
    pthread_mutex_lock(&mutex);
    preferences_load();
    preference_store(name,key,&value);
    pthread_mutex_unlock(&mutex);
    free(key); free(text);
}
void vegas_preference_set(jobject object) {
    pthread_mutex_lock(&mutex);
    Bundle *bundle = bundle_find(object);
    const char *name, *key; int type;
    if (!bundle || preference_request(bundle, &name, &key, &type)) {
        LOG_ERROR("nativeSetPreference: invalid Bundle request");
        pthread_mutex_unlock(&mutex); return;
    }
    preferences_load();
    Entry *data = entry_find(bundle, "npData");
    Value empty = {.type = type, .is_null = type == VALUE_STRING};
    const Value *value = data && (int)data->value.type == type ? &data->value : &empty;
    preference_store(name, key, value);
    pthread_mutex_unlock(&mutex);
}
void vegas_bundle_install(void) {
    if (installed) return;
    saved = *jni;
    struct JNINativeInterface *table = (struct JNINativeInterface *)jni;
    table->GetMethodID = get_method;
    table->GetObjectClass = object_class;
    table->NewObject = new_object; table->NewObjectV = new_object_v; table->NewObjectA = new_object_a;
#define INSTALL_CALL(label) \
    table->Call##label##Method = call_##label; \
    table->Call##label##MethodV = call_##label##_v; \
    table->Call##label##MethodA = call_##label##_a
    INSTALL_CALL(Object); INSTALL_CALL(Int); INSTALL_CALL(Long); INSTALL_CALL(Boolean);
    table->CallVoidMethod = call_void; table->CallVoidMethodV = call_void_v; table->CallVoidMethodA = call_void_a;
    table->NewGlobalRef = new_global; table->DeleteGlobalRef = delete_global;
    table->NewLocalRef = new_local; table->DeleteLocalRef = delete_local;
    installed = 1;
    LOG_SUCCESS("JNI typed Bundle and persistent preferences installed");
}
