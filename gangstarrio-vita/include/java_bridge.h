#ifndef GANGSTARRIO_JAVA_BRIDGE_H
#define GANGSTARRIO_JAVA_BRIDGE_H

#include <falso_jni/FalsoJNI.h>

typedef void (*RioSetPathsCall)(JNIEnv *, jclass, jstring, jstring, jstring);

void java_init(void);
void java_configure_set_paths(RioSetPathsCall callback, jclass clazz);
int java_paths_ready(void);

#endif
