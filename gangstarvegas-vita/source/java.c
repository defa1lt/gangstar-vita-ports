#include "runtime.h"
#include "game_profile.h"
#include "md5.h"
#include "bundle.h"
#include "audio.h"
#include "diagnostics.h"
#include <vitaGL.h>
#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define ROOT "ux0:data/gangstarvegas/"
#define JNI_PREFIX "Java_com_gameloft_android_ANMP_GloftGGHM_"
#define CONTEXT ((jobject)0x42424242)
static const char device_identity[]="PSVITA-GSGV00001";
static SceUID graphics_owner;
static int graphics_ready;
static unsigned context_calls;
static void (*renderer_init)(JNIEnv *, jclass, jint, jint, jint, jstring);
static int view_created;

enum { STR_EMPTY=1, STR_ROOT, STR_FILES, STR_APK, STR_PACKAGE, STR_GAME, STR_VERSION,
       STR_MANUFACTURER, STR_MODEL, STR_CPU, STR_ARCH, STR_GPU, STR_MAIN, STR_PATCH,
       STR_USER_AGENT, STR_COUNTRY, STR_LANGUAGE, STR_CONTEXT, STR_ID,
       PREFERENCE, RESOURCE, ASSET, EMPTY_BYTES, VOID_EMPTY, CREATE_VIEW, VIEW_SETTINGS,
       CURRENT_CONTEXT, BOOLEAN_FALSE, BOOLEAN_TRUE, INT_ZERO, INT_WIDTH, INT_HEIGHT,
       INT_MANUFACTURER, FLOAT_FREQUENCY, FLOAT_MEMORY, TIME_NOW, EXIT_GAME,
       BARRELS, GLUID, META_VALUE, PREFERENCE_GET, PREFERENCE_SET, PREFERENCE_FALLBACK, SHARED_GET, SHARED_SET, SHARED_DELETE, SHARED_HAS,
       GANGSTAR_GET, GANGSTAR_SET, GANGSTAR_DELETE, GANGSTAR_HAS,
       STR_REGION, STR_CONTROLLER, INT_CONTROLLER, INT_XPERIA, INT_API, INT_TEXTURE, HTTP_TEMPLATE, DEBUG_MESSAGE, GLUID_DEFAULT, STR_BRAND_MODEL, STR_UNKNOWN,
       STR_HDIDFV, STR_HDIDFV_VERSION, STR_DEVICE_GLDID };
static jobject string_value(jmethodID id, va_list arguments) {
    (void)arguments;
    const char *value = "";
    char identifier[48];
    switch ((int)id) {
        case STR_BRAND_MODEL: value="Sony_PlayStation Vita"; break;
        case STR_UNKNOWN: value="Unknown"; break;
        case STR_REGION: value="en_US"; break;
        case STR_CONTROLLER: value="PlayStation Vita"; break;
        case STR_ROOT: value=ROOT; break;
        case STR_FILES: value=ROOT "files"; break;
        case STR_APK: value=ROOT "game.apk"; break;
        case STR_PACKAGE: value="com.gameloft.android.ANMP.GloftGGHM"; break;
        case STR_GAME: value="Gangstar Vegas"; break;
        case STR_VERSION: value="1.7.0g"; break;
        case STR_MANUFACTURER: value="Sony"; break;
        case STR_MODEL: value="PlayStation Vita"; break;
        case STR_CPU: value="ARM Cortex-A9"; break;
        case STR_ARCH: value="ARMv7"; break;
        case STR_GPU: value="PowerVR SGX543MP4"; break;
        case STR_MAIN: value="main.17026.com.gameloft.android.ANMP.GloftGGHM.obb"; break;
        case STR_PATCH: value="patch.17026.com.gameloft.android.ANMP.GloftGGHM.obb"; break;
        case STR_USER_AGENT: value="GangstarVegas/1.7.0g (PlayStation Vita; ARMv7)"; break;
        case STR_COUNTRY: value="US"; break;
        case STR_LANGUAGE: value="en"; break;
        case STR_CONTEXT: return CONTEXT;
        case STR_ID: value=device_identity; break;
        case STR_HDIDFV: vegas_md5_hex(device_identity,strlen(device_identity),identifier); value=identifier; break;
        case STR_HDIDFV_VERSION: value="v1.0.0"; break;
        case STR_DEVICE_GLDID:
            strcpy(identifier,"hdidfv=");
            vegas_md5_hex(device_identity,strlen(device_identity),identifier+7);
            strcat(identifier," "); value=identifier; break;
    }
    return jni->NewStringUTF(&jni, value);
}
static char *expand_template(const char *input) {
    char hdidfv[33]; vegas_md5_hex(device_identity,strlen(device_identity),hdidfv);
    const struct { const char *key,*value; } substitutions[]={
        {"SOURCEGAMECODE","GGHM"},{"MARKET","ANMP"},{"GAMEVERSION","1.7.0"},
        {"GC_LETTER","g"},{"LANGCODE","EN"},{"COUNTRYOFDEVICE","US"},
        {"DEVICE_NAME","Sony_PlayStationVita"},{"FIRMWARE","PlayStation Vita"},
        {"UDID",device_identity},{"HDIDFV",hdidfv},{"ANDROID_ID",device_identity}
    };
    if (!input || strlen(input)>1024*1024) return NULL;
    char *text=strdup(input);
    for (size_t i=0;text && i<sizeof(substitutions)/sizeof(substitutions[0]);++i) {
        size_t key_length=strlen(substitutions[i].key),value_length=strlen(substitutions[i].value);
        size_t count=0; const char *cursor=text,*match;
        while ((match=strstr(cursor,substitutions[i].key))) { ++count; cursor=match+key_length; }
        size_t length=strlen(text)-count*key_length+count*value_length;
        if (length>1024*1024) { free(text); return NULL; }
        char *replaced=malloc(length+1),*out=replaced;
        if (!replaced) { free(text); return NULL; }
        cursor=text;
        while ((match=strstr(cursor,substitutions[i].key))) {
            size_t prefix=match-cursor; memcpy(out,cursor,prefix); out+=prefix;
            memcpy(out,substitutions[i].value,value_length); out+=value_length; cursor=match+key_length;
        }
        strcpy(out,cursor); free(text); text=replaced;
    }
    return text;
}
static jobject http_template(jmethodID id, va_list arguments) {
    (void)id;
    jstring input=va_arg(arguments,jstring);
    const char *text=input?jni->GetStringUTFChars(&jni,input,NULL):NULL;
    char *expanded=expand_template(text);
    if (text) jni->ReleaseStringUTFChars(&jni,input,(char *)text);
    if (!expanded) return NULL;
    jobject result=jni->NewStringUTF(&jni,expanded); free(expanded); return result;
}
static jobject preference(jmethodID id, va_list arguments) {
    if ((int)id == PREFERENCE_GET) return vegas_preference_get(va_arg(arguments,jobject));
    jstring key=va_arg(arguments,jstring);
    jstring fallback=(int)id==PREFERENCE_FALLBACK?va_arg(arguments,jstring):NULL;
    jstring namespace=va_arg(arguments,jstring);
    const char *name=namespace?jni->GetStringUTFChars(&jni,namespace,NULL):NULL;
    jobject result=vegas_string_preference_get(name?name:"",key,fallback);
    if (name) jni->ReleaseStringUTFChars(&jni,namespace,(char *)name);
    return result;
}
static const char *shared_namespace(jmethodID id) {
    return (int)id>=GANGSTAR_GET?"DataSharingGangstar":"DataSharing";
}
static jobject shared_get(jmethodID id, va_list arguments) {
    return vegas_string_preference_get(shared_namespace(id),va_arg(arguments,jstring),NULL);
}
static jobject bytes(jmethodID id, va_list arguments) {
    if ((int)id == EMPTY_BYTES) return jni->NewByteArray(&jni, 0);
    jstring name = va_arg(arguments, jstring);
    const char *raw = name ? jni->GetStringUTFChars(&jni, name, NULL) : NULL;
    if (!raw) return NULL;
    const char *relative = raw;
    if (*relative == '.') ++relative;
    while (*relative == '/') ++relative;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s%s%s", ROOT,
                          (int)id == ASSET ? "assets/" : "", relative);
    FILE *file = length > 0 && length < (int)sizeof(path) && !strstr(relative, "..") ? fopen(path,"rb") : NULL;
    jni->ReleaseStringUTFChars(&jni, name, (char *)raw);
    if (!file) { LOG_ERROR("Java resource open failed: %s", path); return NULL; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long size = ftell(file); rewind(file);
    if (size < 0 || size > 16*1024*1024) { fclose(file); return NULL; }
    void *data = malloc(size ? (size_t)size : 1);
    if (!data) { fclose(file); return NULL; }
    size_t count = fread(data, 1, size, file); fclose(file);
    if (count != (size_t)size) { free(data); return NULL; }
    jbyteArray result = jni->NewByteArray(&jni, size);
    if (result && size) jni->SetByteArrayRegion(&jni, result, 0, size, data);
    free(data); return result;
}
static jobject integers(jmethodID id, va_list arguments) {
    jint values[4]={GAME_APK_SIGNATURE_HASH,0,0,0};
    if ((int)id == GLUID || (int)id == GLUID_DEFAULT) {
        jstring input=(int)id==GLUID?va_arg(arguments,jstring):NULL;
        const char *text=(int)id==GLUID_DEFAULT?device_identity:input?jni->GetStringUTFChars(&jni,input,NULL):NULL;
        if (!text) return NULL;
        const char *package="com.gameloft.android.ANMP.GloftGGHM";
        size_t length=strlen(text)+strlen(package);
        char *message=malloc(length+1);
        if (!message) { if (input) jni->ReleaseStringUTFChars(&jni,input,(char *)text); return NULL; }
        strcpy(message,text); strcat(message,package);
        uint8_t digest[16]; vegas_md5(message,length,digest); free(message);
        if (input) jni->ReleaseStringUTFChars(&jni,input,(char *)text);
        for (unsigned i=0;i<4;++i) values[i]=(uint32_t)digest[i*4]<<24 | (uint32_t)digest[i*4+1]<<16 | (uint32_t)digest[i*4+2]<<8 | digest[i*4+3];
    }
    jintArray result=jni->NewIntArray(&jni,4);
    if (result) jni->SetIntArrayRegion(&jni,result,0,4,values);
    return result;
}
static void void_method(jmethodID id, va_list arguments) {
    if ((int)id==SHARED_SET || (int)id==GANGSTAR_SET || (int)id==SHARED_DELETE || (int)id==GANGSTAR_DELETE) {
        jstring key=va_arg(arguments,jstring);
        jstring value=((int)id==SHARED_SET || (int)id==GANGSTAR_SET)?va_arg(arguments,jstring):NULL;
        vegas_string_preference_set(shared_namespace(id),key,value); return;
    }
    if ((int)id==DEBUG_MESSAGE) {
        jstring message=va_arg(arguments,jstring);
        const char *text=message?jni->GetStringUTFChars(&jni,message,NULL):NULL;
        if (text) { LOG_INFO("Vegas: %s",text); jni->ReleaseStringUTFChars(&jni,message,(char *)text); }
        return;
    }
    if ((int)id == PREFERENCE_SET) { vegas_preference_set(va_arg(arguments,jobject)); return; }
    if ((int)id == CREATE_VIEW && !view_created && renderer_init) {
        view_created = 1;
        jstring version = jni->NewStringUTF(&jni, "1.7.0g");
        renderer_init(&jni, jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/GameRenderer"),0,960,544,version);
        jni->DeleteLocalRef(&jni,version);
        LOG_SUCCESS("Java createView: Vegas renderer initialized 960x544");
    } else if ((int)id == VIEW_SETTINGS) {
        jint width = va_arg(arguments,jint), height = va_arg(arguments,jint);
        LOG_INFO("Java setViewSettings: %dx%d", width, height);
    } else if ((int)id == EXIT_GAME) {
        LOG_ERROR("Vegas requested exit");
        exit(0);
    }
}
static jint int_method(jmethodID id, va_list arguments) {
    (void)arguments;
    if ((int)id==INT_CONTROLLER) return 8;
    if ((int)id==INT_XPERIA) return 2;
    if ((int)id==INT_API) return 19;
    if ((int)id==INT_TEXTURE) {
        const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
        if (!extensions) return 0;
        if (strstr(extensions,"GL_IMG_texture_compression_pvrtc")) return 1;
        if (strstr(extensions,"GL_AMD_compressed_ATC_texture")) return 2;
        if (strstr(extensions,"GL_EXT_texture_compression_dxt1")) return 3;
        if (strstr(extensions,"GL_OES_compressed_ETC1_RGB8_texture")) return 4;
        return 0;
    }
    if ((int)id == INT_WIDTH) return 960;
    if ((int)id == INT_HEIGHT) return 544;
    return 0;
}
static jboolean bool_method(jmethodID id, va_list arguments) {
    if ((int)id==SHARED_HAS || (int)id==GANGSTAR_HAS) {
        jstring value=vegas_string_preference_get(shared_namespace(id),va_arg(arguments,jstring),NULL);
        if (!value) return JNI_FALSE;
        jboolean result=jni->GetStringLength(&jni,value)>0;
        jni->DeleteLocalRef(&jni,value); return result;
    }
    if ((int)id == CURRENT_CONTEXT) {
        int index = va_arg(arguments, int);
        SceUID thread=sceKernelGetThreadId();
        int result=index==0 && thread==graphics_owner && graphics_ready;
        unsigned call=__atomic_add_fetch(&context_calls,1,__ATOMIC_RELAXED);
        if (call<=16 || !(call & (call-1)))
            LOG_INFO("Java setCurrentContext: index=%d thread=0x%x owner=0x%x ready=%d result=%d",
                     index,thread,graphics_owner,thread==graphics_owner ? graphics_ready : 0,result);
        return result;
    }
    return (int)id == BOOLEAN_TRUE;
}
static jfloat float_method(jmethodID id, va_list arguments) {
    (void)arguments;
    return (int)id == FLOAT_FREQUENCY ? (float)scePowerGetArmClockFrequency() : 256.0f;
}
static jlong long_method(jmethodID id, va_list arguments) {
    (void)id; (void)arguments;
    struct timeval time; gettimeofday(&time,NULL); return (jlong)time.tv_sec * 1000 + time.tv_usec/1000;
}
#define OBJECT(id, name) {id, name, METHOD_TYPE_OBJECT}
#define INTEGER(id, name) {id, name, METHOD_TYPE_INT}
#define BOOLEAN(id, name) {id, name, METHOD_TYPE_BOOLEAN}
#define VOID(id, name) {id, name, METHOD_TYPE_VOID}
NameToMethodID nameToMethodId[] = {
    OBJECT(AUDIO_CREATE,"android/media/AudioTrack/<init>(IIIIII)V"),
    INTEGER(AUDIO_RATE,"android/media/AudioTrack/getNativeOutputSampleRate(I)I"),
    INTEGER(AUDIO_BUFFER,"android/media/AudioTrack/getMinBufferSize(III)I"),
    INTEGER(AUDIO_WRITE,"android/media/AudioTrack/write([BII)I"),
    INTEGER(AUDIO_STATE,"android/media/AudioTrack/getPlayState()I"),
    VOID(AUDIO_PLAY,"android/media/AudioTrack/play()V"),
    VOID(AUDIO_PAUSE,"android/media/AudioTrack/pause()V"),
    VOID(AUDIO_STOP,"android/media/AudioTrack/stop()V"),
    VOID(AUDIO_RELEASE,"android/media/AudioTrack/release()V"),
#define SHARED_METHODS(prefix,get,set,del,has) \
    OBJECT(get,prefix "/getSharedValue"), VOID(set,prefix "/setSharedValue"), \
    VOID(del,prefix "/deleteSharedValue"), BOOLEAN(has,prefix "/isSharedValue")
    SHARED_METHODS("com/gameloft/android/ANMP/GloftGGHM/DataSharing",SHARED_GET,SHARED_SET,SHARED_DELETE,SHARED_HAS),
    SHARED_METHODS("com/gameloft/android/ANMP/GloftGGHM/DataSharingGangstar",GANGSTAR_GET,GANGSTAR_SET,GANGSTAR_DELETE,GANGSTAR_HAS),
#undef SHARED_METHODS
    OBJECT(STR_EMPTY,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getPhoneCarrier()Ljava/lang/String;"),
    OBJECT(STR_COUNTRY,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getLocaleCountry()Ljava/lang/String;"),
    OBJECT(STR_LANGUAGE,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getLocaleLanguage()Ljava/lang/String;"),
    OBJECT(STR_BRAND_MODEL,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getManufacturerModel()Ljava/lang/String;"),
    OBJECT(STR_UNKNOWN,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getCurrentRuntimeValue()Ljava/lang/String;"),
    OBJECT(STR_EMPTY,"GetAccessToken"), OBJECT(STR_EMPTY,"getAccessToken"),
    OBJECT(STR_EMPTY,"IsAppLaunchedFromPN"), OBJECT(EMPTY_BYTES,"GetDataFromURL"),
    BOOLEAN(BOOLEAN_FALSE,"IsLoggedIn"),
    OBJECT(HTTP_TEMPLATE,"getHttpResponse"), OBJECT(STR_ID,"getIdentifier"), OBJECT(STR_EMPTY,"getMac"),
    OBJECT(STR_REGION,"GetPhoneRegionFormat"), OBJECT(STR_CONTROLLER,"GetControllerName"),
    OBJECT(STR_EMPTY,"GetVirtualKeyboardText"),
    INTEGER(INT_CONTROLLER,"GetControllerNumber"), INTEGER(INT_XPERIA,"GetXperiaSlideout"),
    INTEGER(INT_API,"GetFirmwareApi"), INTEGER(INT_TEXTURE,"GetTextureFormat"),
    INTEGER(INT_ZERO,"GetPhoneLanguage"), INTEGER(INT_ZERO,"getManufacture"),
    INTEGER(INT_ZERO,"hasAccelerometer"), INTEGER(INT_ZERO,"isRooted"),
    BOOLEAN(BOOLEAN_FALSE,"IsKeyboardVisible"), BOOLEAN(BOOLEAN_FALSE,"playVideo"),
    VOID(VOID_EMPTY,"stopVideo"), VOID(VOID_EMPTY,"inGameVideoSetSkipEnabled"), VOID(DEBUG_MESSAGE,"PrintDebug"),
    OBJECT(STR_CONTEXT,"getContext"), OBJECT(STR_APK,"GetApkPath"), OBJECT(STR_PACKAGE,"getPackage"),
    OBJECT(STR_GAME,"getGameName"), OBJECT(STR_ROOT,"getSDFolder"), OBJECT(STR_ROOT,"GetObbPath"),
    OBJECT(STR_ROOT,"GetExternalObbPath"), OBJECT(STR_FILES,"getSaveFolder"),
    OBJECT(STR_MAIN,"GetMainObbName"), OBJECT(STR_PATCH,"GetPatchObbName"),
    OBJECT(STR_EMPTY,"GetExtraObbName"), OBJECT(STR_EMPTY,"getDefaultIGP"),
    OBJECT(STR_EMPTY,"getInjectedIGP"), OBJECT(STR_EMPTY,"getInjectedSerialKey"),
    OBJECT(STR_USER_AGENT,"getUserAgent"), OBJECT(STR_MANUFACTURER,"GetPhoneManufacturer"),
    OBJECT(STR_MANUFACTURER,"getPhoneManufacturer"), OBJECT(STR_MODEL,"GetPhoneModel"),
    OBJECT(STR_MODEL,"getPhoneModel"), OBJECT(STR_MODEL,"getDeviceName"),
    OBJECT(STR_MODEL,"getPhoneDevice"), OBJECT(STR_MODEL,"getPhoneProduct"),
    OBJECT(STR_CPU,"GetPhoneCPUName"), OBJECT(STR_ARCH,"GetPhoneCPUArch"),
    OBJECT(STR_CPU,"GetPhoneCPUPart"), OBJECT(STR_GPU,"GetPhoneGPUName"),
    OBJECT(STR_MODEL,"GetPhoneFirmware"), OBJECT(STR_MODEL,"getDeviceFirmware"),
    OBJECT(STR_ID,"d1"), OBJECT(STR_ID,"getAndroidId"), OBJECT(STR_ID,"getSerial"),
    OBJECT(STR_ID,"getSerialNo"), OBJECT(STR_HDIDFV,"getHDIDFV"), OBJECT(STR_HDIDFV_VERSION,"getHDIDFVVersion"),
    OBJECT(STR_DEVICE_GLDID,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/Device/getGLDID()Ljava/lang/String;"),
    OBJECT(STR_ID,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getGLDID()Ljava/lang/String;"),
    OBJECT(STR_EMPTY,"getMacAddress"), OBJECT(STR_EMPTY,"getDeviceIMEI"),
    OBJECT(STR_EMPTY,"getGoogleAdId"), OBJECT(STR_EMPTY,"retrieveDeviceCarrier"),
    OBJECT(STR_COUNTRY,"retrieveDeviceCountry"), OBJECT(STR_COUNTRY,"retrieveDeviceRegion"),
    OBJECT(STR_LANGUAGE,"retrieveDeviceLanguage"), OBJECT(STR_ID,"retrieveCPUSerial"),
    OBJECT(PREFERENCE_FALLBACK,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/SUtils/getPreferenceString(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
    OBJECT(PREFERENCE,"getPreferenceString"), OBJECT(RESOURCE,"getResource"),
    OBJECT(ASSET,"getAssetAsString"), OBJECT(EMPTY_BYTES,"getKeyboardText"),
    OBJECT(BARRELS,"retrieveBarrels"), OBJECT(GLUID,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/SUtils/getGLUID(Ljava/lang/String;)[I"),
    OBJECT(GLUID_DEFAULT,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getGLUID()[I"),
    OBJECT(STR_EMPTY,"getMetaDataValue"), OBJECT(PREFERENCE_GET,"nativeGetPreference"),
    INTEGER(INT_ZERO,"isWifiEnabled"), INTEGER(INT_ZERO,"hasAnyInternetConnection"),
    INTEGER(INT_ZERO,"hasInternetViaWifi"), INTEGER(INT_ZERO,"hasInternetViaMobile"),
    INTEGER(INT_ZERO,"CheckConnectionType"), INTEGER(INT_ZERO,"hasConnectivity"),
    INTEGER(INT_ZERO,"isXperia"), INTEGER(INT_ZERO,"initCheckConnectionType"),
    INTEGER(INT_ZERO,"getGoogleAdIdStatus"), INTEGER(INT_ZERO,"isReturnkeyboard"),
    INTEGER(INT_ZERO,"isKeyboardRequired"), INTEGER(INT_WIDTH,"getWidth"), INTEGER(INT_HEIGHT,"getHeight"),
    {FLOAT_FREQUENCY,"GetPhoneCPUFreq",METHOD_TYPE_FLOAT},
    {FLOAT_MEMORY,"GetMemoryInfo",METHOD_TYPE_FLOAT}, {TIME_NOW,"GetCurrentTime",METHOD_TYPE_LONG},
    {TIME_NOW,"currentTimeMillis",METHOD_TYPE_LONG},
    BOOLEAN(BOOLEAN_FALSE,"isConnected"), BOOLEAN(BOOLEAN_FALSE,"isNetworkConnected"),
    BOOLEAN(BOOLEAN_FALSE,"IsConnectionReady"), BOOLEAN(BOOLEAN_FALSE,"IsWifiEnable"),
    BOOLEAN(BOOLEAN_FALSE,"IsWifiEnabling"), BOOLEAN(BOOLEAN_FALSE,"IsWifiDisabling"),
    BOOLEAN(CURRENT_CONTEXT,"setCurrentContext"),
    VOID(CREATE_VIEW,"createView"), VOID(VIEW_SETTINGS,"setViewSettings"), VOID(VOID_EMPTY,"setupPaths"),
    VOID(VOID_EMPTY,"EnableWifi"), VOID(VOID_EMPTY,"DisableWifi"), VOID(VOID_EMPTY,"enableWifi"),
    VOID(VOID_EMPTY,"com/gameloft/android/ANMP/GloftGGHM/GL2JNILib/enableAccelerometer(ZF)V"),
    VOID(VOID_EMPTY,"com/gameloft/android/ANMP/GloftGGHM/Game/enableAccelerometer(I)V"), VOID(PREFERENCE_SET,"nativeSetPreference"),
    VOID(VOID_EMPTY,"setKeyboard"), VOID(VOID_EMPTY,"setKeyboardText"),
    VOID(EXIT_GAME,"Exit"), VOID(EXIT_GAME,"CloseGame"),
};
MethodsObject methodsObject[] = {
    {AUDIO_CREATE,vegas_audio_create},
    {SHARED_GET,shared_get},{GANGSTAR_GET,shared_get},{HTTP_TEMPLATE,http_template},
    {STR_REGION,string_value},{STR_CONTROLLER,string_value},{STR_BRAND_MODEL,string_value},{STR_UNKNOWN,string_value},
    {GLUID_DEFAULT,integers},
    {STR_EMPTY,string_value},{STR_ROOT,string_value},{STR_FILES,string_value},{STR_APK,string_value},
    {STR_PACKAGE,string_value},{STR_GAME,string_value},{STR_VERSION,string_value},{STR_MANUFACTURER,string_value},
    {STR_MODEL,string_value},{STR_CPU,string_value},{STR_ARCH,string_value},{STR_GPU,string_value},
    {STR_MAIN,string_value},{STR_PATCH,string_value},{STR_USER_AGENT,string_value},{STR_COUNTRY,string_value},
    {STR_LANGUAGE,string_value},{STR_CONTEXT,string_value},{STR_ID,string_value},
    {STR_HDIDFV,string_value},{STR_HDIDFV_VERSION,string_value},{STR_DEVICE_GLDID,string_value},
    {PREFERENCE_FALLBACK,preference},{PREFERENCE,preference},{PREFERENCE_GET,preference},{RESOURCE,bytes},{ASSET,bytes},{EMPTY_BYTES,bytes},{BARRELS,integers},{GLUID,integers},
};
MethodsInt methodsInt[] = {{AUDIO_RATE,vegas_audio_integer},{AUDIO_BUFFER,vegas_audio_integer},
    {AUDIO_WRITE,vegas_audio_integer},{AUDIO_STATE,vegas_audio_integer},
    {INT_CONTROLLER,int_method},{INT_XPERIA,int_method},{INT_API,int_method},{INT_TEXTURE,int_method},{INT_ZERO,int_method},{INT_WIDTH,int_method},{INT_HEIGHT,int_method}};
MethodsBoolean methodsBoolean[] = {{SHARED_HAS,bool_method},{GANGSTAR_HAS,bool_method},{BOOLEAN_FALSE,bool_method},{BOOLEAN_TRUE,bool_method},{CURRENT_CONTEXT,bool_method}};
MethodsFloat methodsFloat[] = {{FLOAT_FREQUENCY,float_method},{FLOAT_MEMORY,float_method}};
MethodsLong methodsLong[] = {{TIME_NOW,long_method}};
MethodsVoid methodsVoid[] = {{AUDIO_PLAY,vegas_audio_void},{AUDIO_PAUSE,vegas_audio_void},
    {AUDIO_STOP,vegas_audio_void},{AUDIO_RELEASE,vegas_audio_void},
    {DEBUG_MESSAGE,void_method},{SHARED_SET,void_method},{GANGSTAR_SET,void_method},{SHARED_DELETE,void_method},{GANGSTAR_DELETE,void_method},{PREFERENCE_SET,void_method},{CREATE_VIEW,void_method},{VIEW_SETTINGS,void_method},{VOID_EMPTY,void_method},{EXIT_GAME,void_method}};
MethodsByte methodsByte[] = {}; MethodsChar methodsChar[] = {}; MethodsDouble methodsDouble[] = {}; MethodsShort methodsShort[] = {};
NameToFieldID nameToFieldId[] = {};
FieldsBoolean fieldsBoolean[] = {}; FieldsByte fieldsByte[] = {}; FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {}; FieldsFloat fieldsFloat[] = {}; FieldsInt fieldsInt[] = {};
FieldsLong fieldsLong[] = {}; FieldsObject fieldsObject[] = {}; FieldsShort fieldsShort[] = {};
__FALSOJNI_IMPL_CONTAINER_SIZES

static void (*touch_event)(JNIEnv *,jclass,jint,jint,jint,jint);
static void (*controller_connected)(JNIEnv *,jclass,jint);
static void (*key_down)(JNIEnv *,jclass,jint,jint);
static void (*key_up)(JNIEnv *,jclass,jint,jint);
static void (*joy_move)(JNIEnv *,jclass,jint,jint,jint,jint);
static int stick_axis(unsigned char value) {
    int delta=(int)value-128;
    int packed=delta*255/(delta<0?128:127);
    return abs(packed)<32 ? 0 : packed;
}
static void poll_controller(jclass game) {
    static const struct { unsigned button; int key; } keys[]={
        {SCE_CTRL_CROSS,96},{SCE_CTRL_CIRCLE,97},{SCE_CTRL_SQUARE,99},{SCE_CTRL_TRIANGLE,100},
        {SCE_CTRL_LTRIGGER,102},{SCE_CTRL_RTRIGGER,103},{SCE_CTRL_START,108},{SCE_CTRL_SELECT,109},
        {SCE_CTRL_UP,19},{SCE_CTRL_DOWN,20},{SCE_CTRL_LEFT,21},{SCE_CTRL_RIGHT,22},
    };
    static unsigned previous_buttons;
    static int previous_axes[4]={256,256,256,256};
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0,&pad,1)<=0) return;
    unsigned changed=pad.buttons^previous_buttons;
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        if (!(changed&keys[i].button)) continue;
        if (pad.buttons&keys[i].button) key_down(&jni,game,keys[i].key,1);
        else key_up(&jni,game,keys[i].key,1);
    }
    previous_buttons=pad.buttons;
    int axes[4]={stick_axis(pad.lx),-stick_axis(pad.ly),stick_axis(pad.rx),stick_axis(pad.ry)};
    for (int stick=0;stick<2;++stick) {
        int at=2*stick;
        if (axes[at]!=previous_axes[at] || axes[at+1]!=previous_axes[at+1])
            joy_move(&jni,game,axes[at],axes[at+1],0,stick);
    }
    memcpy(previous_axes,axes,sizeof(axes));
}
static SceTouchPanelInfo touch_panel;
static struct { int id,x,y; } touches[2]={{-1,0,0},{-1,0,0}};
static void poll_touch(jclass game) {
    SceTouchData data;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT,&data,1)<=0) return;
    for (unsigned slot=0;slot<2;++slot) {
        if (touches[slot].id<0) continue;
        unsigned i=0;
        while (i<data.reportNum && data.report[i].id!=touches[slot].id) ++i;
        if (i==data.reportNum) {
            touch_event(&jni,game,0,touches[slot].x,touches[slot].y,slot);
            touches[slot].id=-1;
        }
    }
    for (unsigned i=0;i<data.reportNum && i<SCE_TOUCH_MAX_REPORT;++i) {
        unsigned slot=0;
        while (slot<2 && touches[slot].id!=data.report[i].id) ++slot;
        int action=2;
        if (slot==2) {
            slot=0; while (slot<2 && touches[slot].id>=0) ++slot;
            if (slot==2) continue;
            touches[slot].id=data.report[i].id; action=1;
        }
        int x=(data.report[i].x-touch_panel.minDispX)*959/(touch_panel.maxDispX-touch_panel.minDispX);
        int y=(data.report[i].y-touch_panel.minDispY)*543/(touch_panel.maxDispY-touch_panel.minDispY);
        if (x<0) x=0;
        if (x>959) x=959;
        if (y<0) y=0;
        if (y>543) y=543;
        if (action==1 || x!=touches[slot].x || y!=touches[slot].y)
            touch_event(&jni,game,action,x,y,slot);
        touches[slot].x=x; touches[slot].y=y;
    }
}

static void log_native_thread(const char *phase) {
    const unsigned char *node=(void *)so_symbol(&so_mod,"_ZN3glf6Thread10mNativeTlsE");
    const unsigned *mask=(void *)so_symbol(&so_mod,"_ZN3glf6Thread12usedThreadIdE");
    if (!node || !mask) { LOG_ERROR("Native thread state symbols missing"); return; }
    pthread_key_t key; memcpy(&key,node+8,sizeof(key));
    const unsigned *value=pthread_getspecific(key);
    LOG_INFO("Native thread state %s: tls=%p sequential=%u used=0x%x thread=0x%x",
             phase,value,value ? *value : 0,__atomic_load_n(mask,__ATOMIC_RELAXED),sceKernelGetThreadId());
}

static void *(*native_player)(void);
static void *(*native_player_position)(float *, const void *);
static const void *(*native_player_body)(const void *);
static const uint8_t *(*native_havok_body)(const void *, const void *);
static int (*native_streamer_done)(const void *);
static int (*native_player_vehicle)(const void *);
static int (*native_district_loaded)(const void *, const float *);
static const uint8_t *volatile *native_physics_world;
int vegas_bind_player_probe(void) {
    const uint32_t *get=(void *)(so_mod.text_base+GAME_PLAYER_STATE_GET_PLAYER);
    const uint32_t *position=(void *)(so_mod.text_base+GAME_PLAYER_STATE_GET_POSITION);
    const uint32_t *vtable=(void *)(so_mod.text_base+GAME_PLAYER_STATE_VPTR);
    if (get[0]!=GAME_PLAYER_STATE_GET_PLAYER_WORD0 || get[1]!=GAME_PLAYER_STATE_GET_PLAYER_WORD1 ||
        position[0]!=GAME_PLAYER_STATE_GET_POSITION_WORD0 || position[1]!=GAME_PLAYER_STATE_GET_POSITION_WORD1 ||
        vtable[0x34/4]!=so_mod.text_base+GAME_PLAYER_STATE_GET_POSITION) {
        LOG_ERROR("Player state probe profile mismatch"); return -1;
    }
    const uint32_t guards[][3]={
        {GAME_PLAYER_STATE_IN_VEHICLE,GAME_PLAYER_STATE_IN_VEHICLE_WORD0,GAME_PLAYER_STATE_IN_VEHICLE_WORD1},
        {GAME_PHYSICS_STATE_GET_PHYSICS_BODY,GAME_PHYSICS_STATE_GET_PHYSICS_BODY_WORD0,GAME_PHYSICS_STATE_GET_PHYSICS_BODY_WORD1},
        {GAME_PHYSICS_STATE_GET_HKP_BODY,GAME_PHYSICS_STATE_GET_HKP_BODY_WORD0,GAME_PHYSICS_STATE_GET_HKP_BODY_WORD1},
        {GAME_PHYSICS_STATE_STREAMER_DONE,GAME_PHYSICS_STATE_STREAMER_DONE_WORD0,GAME_PHYSICS_STATE_STREAMER_DONE_WORD1},
        {GAME_PHYSICS_STATE_STREAMER_DISTRICT_LOADED,GAME_PHYSICS_STATE_STREAMER_DISTRICT_LOADED_WORD0,GAME_PHYSICS_STATE_STREAMER_DISTRICT_LOADED_WORD1}
    };
    for (unsigned i=0;i<sizeof(guards)/sizeof(guards[0]);++i) {
        const uint32_t *entry=(void *)(so_mod.text_base+guards[i][0]);
        if (entry[0]!=guards[i][1] || entry[1]!=guards[i][2]) {
            LOG_ERROR("Player physics probe profile mismatch"); return -1;
        }
    }
    native_player_vehicle=(void *)(so_mod.text_base+GAME_PLAYER_STATE_IN_VEHICLE);
    native_player_body=(void *)(so_mod.text_base+GAME_PHYSICS_STATE_GET_PHYSICS_BODY);
    native_havok_body=(void *)(so_mod.text_base+GAME_PHYSICS_STATE_GET_HKP_BODY);
    native_streamer_done=(void *)(so_mod.text_base+GAME_PHYSICS_STATE_STREAMER_DONE);
    native_district_loaded=(void *)(so_mod.text_base+GAME_PHYSICS_STATE_STREAMER_DISTRICT_LOADED);
    native_physics_world=(void *)(so_mod.text_base+GAME_PHYSICS_STATE_WORLD_SLOT);
    native_player=(void *)get;
    native_player_position=(void *)position;
    return 0;
}
static const uint8_t *physics_pointer(const uint8_t *object,unsigned offset) {
    return *(const uint8_t *const *)(object+offset);
}
static void trace_player_physics(unsigned frame,const void *player,const float *position,int finite) {
    static const void *anchor_player;
    static float anchor[3];
    const uint8_t *world=*native_physics_world;
    if (!world) { LOG_INFO("Physics probe: frame=%u world=absent",frame); return; }
    const uint8_t *havok=physics_pointer(world,GAME_PHYSICS_STATE_WORLD_HAVOK);
    const uint8_t *streamer=physics_pointer(world,GAME_PHYSICS_STATE_WORLD_STREAMER);
    const uint8_t *grid=streamer?physics_pointer(streamer,GAME_PHYSICS_STATE_STREAMER_GRID):NULL;
    uintptr_t begin=(uintptr_t)physics_pointer(world,GAME_PHYSICS_STATE_WORLD_BODY_BEGIN);
    uintptr_t end=(uintptr_t)physics_pointer(world,GAME_PHYSICS_STATE_WORLD_BODY_END);
    uintptr_t span=end-begin;
    int bodies=end>=begin && !(span%sizeof(void *)) && span/sizeof(void *)<=INT32_MAX
        ? (int)(span/sizeof(void *)) : -1;
    const uint8_t *fixed=havok?physics_pointer(havok,GAME_PHYSICS_STATE_FIXED_ISLAND):NULL;
    int fixed_count=fixed?*(const int *)(fixed+GAME_PHYSICS_STATE_ISLAND_ENTITY_COUNT):-1;
    int done=grid?native_streamer_done(streamer):-1;
    int district=-1,anchor_district=-1;
    if (finite) {
        float dx=position[0]-anchor[0],dy=position[1]-anchor[1];
        if (anchor_player!=player || dx*dx+dy*dy>1000000.0f) {
            anchor_player=player;
            for (unsigned i=0;i<3;++i) anchor[i]=position[i];
            LOG_INFO("Physics anchor: frame=%u xyz=(%.3f,%.3f,%.3f)",frame,anchor[0],anchor[1],anchor[2]);
        }
        if (grid) {
            district=native_district_loaded(streamer,position);
            anchor_district=native_district_loaded(streamer,anchor);
        }
    }
    // This getter adjusts the factory reference to the PhysicsBody C++ base.
    const void *physics=native_player_body(player);
    const uint8_t *body=physics?native_havok_body(world,physics):NULL;
    const uint8_t *body_world=body?physics_pointer(body,GAME_PHYSICS_STATE_BODY_WORLD):NULL;
    const uint8_t *shape=body?physics_pointer(body,GAME_PHYSICS_STATE_BODY_SHAPE):NULL;
    int motion=body?body[GAME_PHYSICS_STATE_BODY_MOTION_TYPE]:-1;
    int member=body && havok && body_world==havok;
    int vehicle=native_player_vehicle(player);
    LOG_INFO("Physics probe: frame=%u world=%p havok=%p streamer=%p grid=%p bodies=%d fixed=%d done=%d district=%d anchor_district=%d physics=%p body=%p body_world=%p shape=%p motion=%d member=%d vehicle=%d",
             frame,world,havok,streamer,grid,bodies,fixed_count,done,district,anchor_district,
             physics,body,body_world,shape,motion,member,vehicle);
}
void vegas_trace_player_state(unsigned frame) {
    static unsigned samples;
    static unsigned status_samples, last_status;
    static const void *previous_player;
    static float previous_z;
    if ((frame!=1 && frame%300) || samples>=32) return;
    const uint8_t *player=native_player();
    if (!player) {
        previous_player=NULL;
        if (last_status!=1 && status_samples<8) {
            ++status_samples;
            LOG_INFO("Player probe: frame=%u absent",frame);
        }
        last_status=1; return;
    }
    uintptr_t vptr=*(const uint32_t *)player;
    if (vptr!=so_mod.text_base+GAME_PLAYER_STATE_VPTR) {
        previous_player=NULL;
        if (last_status!=2 && status_samples<8) {
            ++status_samples;
            LOG_INFO("Player probe: frame=%u player=%p unknown_vptr=0x%x",frame,player,(unsigned)vptr);
        }
        last_status=2; return;
    }
    const uint8_t *node=*(const void *const *)(player+GAME_PLAYER_STATE_NODE);
    unsigned flags=*(const uint32_t *)(player+GAME_PLAYER_STATE_FLAGS);
    unsigned node_flags=node?*(const uint32_t *)(node+GAME_PLAYER_STATE_NODE_FLAGS):0;
    const void *physics=*(const void *const *)(player+GAME_PLAYER_STATE_PHYSICS_REFERENCE);
    const void *ragdoll=*(const void *const *)(player+GAME_PLAYER_STATE_RAGDOLL);
    int fall=*(const int *)(player+GAME_PLAYER_STATE_FALL_STATE);
    if (!node) {
        previous_player=NULL;
        if (last_status!=3 && status_samples<8) {
            ++status_samples;
            LOG_INFO("Player probe: frame=%u player=%p no_node hide=0x%x fall=%d physics=%p ragdoll=%p",
                     frame,player,flags&0x400,fall,physics,ragdoll);
        }
        last_status=3; return;
    }
    last_status=4;
    ++samples;
    // Native vector3 uses a hidden 12-byte output; world vertical is Z.
    float position[3]={NAN,NAN,NAN};
    void *result=native_player_position(position,player);
    if (result!=position) {
        previous_player=NULL;
        LOG_ERROR("Player probe: frame=%u native position result=%p",frame,result); return;
    }
    int finite=isfinite(position[0])&&isfinite(position[1])&&isfinite(position[2]);
    int previous=finite&&previous_player==player;
    float dz=previous?position[2]-previous_z:0;
    LOG_INFO("Player probe: frame=%u player=%p xyz=(%.3f,%.3f,%.3f) finite=%d dz=%.3f previous=%d hide=0x%x node=%p node_flags=0x%x fall=%d physics=%p ragdoll=%p",
             frame,player,position[0],position[1],position[2],finite,dz,previous,
             flags&0x400,node,node_flags,fall,physics,ragdoll);
    trace_player_physics(frame,player,position,finite);
    if (finite) { previous_player=player; previous_z=position[2]; }
    else previous_player=NULL;
}

int vegas_java_start(void) {
    if (vegas_bind_player_probe()<0) return -1;
    graphics_owner = sceKernelGetThreadId();
    graphics_ready=0;
    jni_init();
    vegas_bundle_install();
    jint (*onload)(JavaVM *,void *) = (void *)so_symbol(&so_mod,"JNI_OnLoad");
    renderer_init = (void *)so_symbol(&so_mod,JNI_PREFIX "GameRenderer_nativeInit");
    if (!onload || !renderer_init || onload(&jvm,NULL) != JNI_VERSION_1_4) { LOG_ERROR("JNI_OnLoad failed"); return -1; }
    struct { const char *symbol; const char *class; } calls[] = {
        {JNI_PREFIX "SendInfo_initMethods","com/gameloft/android/ANMP/GloftGGHM/SendInfo"},
        {JNI_PREFIX "GLUtils_SUtils_nativeInit","com/gameloft/android/ANMP/GloftGGHM/GLUtils/SUtils"},
        {JNI_PREFIX "GLUtils_Device_nativeInit","com/gameloft/android/ANMP/GloftGGHM/GLUtils/Device"},
        {JNI_PREFIX "PushNotification_SimplifiedAndroidUtils_nativeInit","com/gameloft/android/ANMP/GloftGGHM/PushNotification/SimplifiedAndroidUtils"},
        {"Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit","com/gameloft/GLSocialLib/PlatformAndroid"},
        {"Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeInit","com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib"},
        {"Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit","com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib"},
        {JNI_PREFIX "GL2JNILib_init","com/gameloft/android/ANMP/GloftGGHM/GL2JNILib"},
        {JNI_PREFIX "Game_nativeInit","com/gameloft/android/ANMP/GloftGGHM/Game"},
        {JNI_PREFIX "DataSharing_nativeInit","com/gameloft/android/ANMP/GloftGGHM/DataSharing"},
        {JNI_PREFIX "DataSharingGangstar_nativeInit","com/gameloft/android/ANMP/GloftGGHM/DataSharingGangstar"},
    };
    for (size_t i=0;i<sizeof(calls)/sizeof(calls[0]);++i) {
        void (*method)(JNIEnv *,jclass)=(void *)so_symbol(&so_mod,calls[i].symbol);
        if (!method) { LOG_ERROR("missing JNI export: %s",calls[i].symbol); return -1; }
        LOG_INFO("JNI enter %s",calls[i].symbol);
        method(&jni,jni->FindClass(&jni,calls[i].class));
        LOG_SUCCESS("JNI returned %s",calls[i].symbol);
    }
    jclass game=jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/Game");
    void (*assets)(JNIEnv *,jclass,jobject)=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_InitNativeAssetManager");
    if (!assets) return -1;
    assets(&jni,game,CONTEXT);
    jclass gl=jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/GL2JNILib");
    void (*settings)(JNIEnv *,jclass)=(void *)so_symbol(&so_mod,JNI_PREFIX "GL2JNILib_getViewSettings");
    void (*resize)(JNIEnv *,jclass,jint,jint)=(void *)so_symbol(&so_mod,JNI_PREFIX "GL2JNILib_resize");
    void (*state)(JNIEnv *,jclass,jboolean)=(void *)so_symbol(&so_mod,JNI_PREFIX "GL2JNILib_stateChanged");
    void (*step)(JNIEnv *,jclass)=(void *)so_symbol(&so_mod,JNI_PREFIX "GL2JNILib_step");
    if (!settings || !resize || !step || !state) return -1;
    LOG_INFO("JNI enter getViewSettings / App::MyInit"); settings(&jni,gl); LOG_SUCCESS("JNI returned getViewSettings");
    log_native_thread("after settings");
    // Android chooses EGLConfig in getViewSettings before its contexts exist.
    graphics_ready=1;
    state(&jni,gl,JNI_TRUE); resize(&jni,gl,960,544);
    log_native_thread("after resize");
    touch_event=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_nativeOnTouch");
    controller_connected=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_nativeControllerConnected");
    key_down=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_nativeOnKeyDown");
    key_up=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_nativeOnKeyUp");
    joy_move=(void *)so_symbol(&so_mod,JNI_PREFIX "Game_nativeJoyPadMove");
    if (!controller_connected || !key_down || !key_up || !joy_move ||
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG)<0) return -1;
    if (!touch_event || sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT,&touch_panel)<0 ||
        touch_panel.maxDispX<=touch_panel.minDispX || touch_panel.maxDispY<=touch_panel.minDispY ||
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,SCE_TOUCH_SAMPLING_STATE_START)<0) return -1;
    LOG_SUCCESS("Entering Vegas frame loop");
    unsigned frames=0;
    diagnostics_watch_start();
    for (;;) {
        poll_touch(game);
        if (frames) poll_controller(game);
        if (!frames) LOG_INFO("First frame: enter game step");
        step(&jni,gl);
        if (!frames) LOG_SUCCESS("First frame: game step returned; native presentations=%u",vegas_presentations());
        if (++frames==1) {
            diagnostics_watch_stop();
            controller_connected(&jni,game,8);
            LOG_SUCCESS("Vita controller connected using the native generic profile");
        }
        if (frames==1 || frames%300==0) LOG_SUCCESS("Vegas frame=%u native presentations=%u",frames,vegas_presentations());
        vegas_trace_player_state(frames);
    }
}
