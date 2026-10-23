#pragma once
#include <falso_jni/FalsoJNI.h>

void vegas_bundle_install(void);
jobject vegas_preference_get(jobject bundle);
void vegas_preference_set(jobject bundle);

jstring vegas_string_preference_get(const char *name, jstring key, jstring fallback);
void vegas_string_preference_set(const char *name, jstring key, jstring value);
