/* Run in an empty scratch directory: ./bundle-test, then ./bundle-test reload.
 * Links the production Bundle, generated corrected FalsoJNI, bridge and converter.
 * Only logging is replaced; objects, JNI calls and file I/O use production code. */
#include "bundle.h"
#include "md5.h"
#include <falso_jni/FalsoJNI_Impl.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "java-integers-under-test.c"
#include "java-strings-under-test.c"
#define TABLE(T,n) T n[] = {}
static jint ordinary(jmethodID id, va_list args) {(void)id; (void)args; return 777;}
NameToMethodID nameToMethodId[] = {{1, "ordinary", METHOD_TYPE_INT},
 {2,"example/One/sameName",METHOD_TYPE_OBJECT},{3,"example/Two/sameName",METHOD_TYPE_OBJECT},
 {4,"example/One/sameName(Ljava/lang/String;)Ljava/lang/String;",METHOD_TYPE_OBJECT},
 {GLUID,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/SUtils/getGLUID(Ljava/lang/String;)[I",METHOD_TYPE_OBJECT},
 {GLUID_DEFAULT,"com/gameloft/android/ANMP/GloftGGHM/SendInfo/getGLUID()[I",METHOD_TYPE_OBJECT},
#include "java-string-bindings-under-test.h"
};
TABLE(NameToFieldID,nameToFieldId);
TABLE(MethodsBoolean,methodsBoolean); TABLE(MethodsByte,methodsByte); TABLE(MethodsChar,methodsChar);
TABLE(MethodsDouble,methodsDouble); TABLE(MethodsFloat,methodsFloat);
MethodsInt methodsInt[] = {{1, ordinary}};
MethodsObject methodsObject[]={{GLUID,integers},{GLUID_DEFAULT,integers},
 {STR_ID,string_value},{STR_HDIDFV,string_value},{STR_HDIDFV_VERSION,string_value},{STR_DEVICE_GLDID,string_value}};
TABLE(MethodsLong,methodsLong); TABLE(MethodsShort,methodsShort); TABLE(MethodsVoid,methodsVoid);
TABLE(FieldsBoolean,fieldsBoolean); TABLE(FieldsByte,fieldsByte); TABLE(FieldsChar,fieldsChar);
TABLE(FieldsDouble,fieldsDouble); TABLE(FieldsFloat,fieldsFloat); TABLE(FieldsInt,fieldsInt);
TABLE(FieldsLong,fieldsLong); TABLE(FieldsObject,fieldsObject); TABLE(FieldsShort,fieldsShort);
__FALSOJNI_IMPL_CONTAINER_SIZES
#define LOGFN(n) void n(const char*a,int b,const char*c,const char*d,...) {(void)a;(void)b;(void)c;(void)d;}
LOGFN(_fjni_log_info) LOGFN(_fjni_log_warn) LOGFN(_fjni_log_debug) LOGFN(_fjni_log_error)
void diagnostics_log(const char*l,const char*f,...) {va_list a;va_start(a,f);fprintf(stderr,"%s: ",l);vfprintf(stderr,f,a);fputc('\n',stderr);va_end(a);}
static jmethodID method(jclass c,const char*n,const char*s) {jmethodID m=jni->GetMethodID(&jni,c,n,s);assert(m);return m;}
static jstring string(const char*s) {jstring x=jni->NewStringUTF(&jni,s);assert(x);return x;}
static void check_identity(jclass c,const char *name,const char *expected) {
 jmethodID m=jni->GetStaticMethodID(&jni,c,name,"()Ljava/lang/String;");assert(m);
 jstring value=jni->CallStaticObjectMethod(&jni,c,m);assert(value);
 const char *text=jni->GetStringUTFChars(&jni,value,NULL);assert(text && !strcmp(text,expected));
 jni->ReleaseStringUTFChars(&jni,value,(char *)text);jni->DeleteLocalRef(&jni,value);
}
static jint get_int_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jint v=jni->CallIntMethodV(&jni,o,m,a);va_end(a);return v;}
static void put_long_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jni->CallVoidMethodV(&jni,o,m,a);va_end(a);}
static jobject request(jclass c,const char*name,const char*key,int type) {
 jobject b=jni->NewObject(&jni,c,method(c,"<init>","()V"));assert(b);
 jmethodID ps=method(c,"putString","(Ljava/lang/String;Ljava/lang/String;)V");
 jni->CallVoidMethod(&jni,b,ps,string("npPrefName"),string(name));
 jni->CallVoidMethod(&jni,b,ps,string("npKey"),string(key));
 jni->CallVoidMethod(&jni,b,method(c,"putInt","(Ljava/lang/String;I)V"),string("npDataType"),type);
 return b;
}
static jobject make_v(jclass c,jmethodID m,...) {va_list a;va_start(a,m);jobject o=jni->NewObjectV(&jni,c,m,a);va_end(a);return o;}
static jobject object_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jobject r=jni->CallObjectMethodV(&jni,o,m,a);va_end(a);return r;}
static jlong long_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jlong r=jni->CallLongMethodV(&jni,o,m,a);va_end(a);return r;}
static jboolean bool_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jboolean r=jni->CallBooleanMethodV(&jni,o,m,a);va_end(a);return r;}
static void void_v(jobject o,jmethodID m,...) {va_list a;va_start(a,m);jni->CallVoidMethodV(&jni,o,m,a);va_end(a);}
static void *thread_test(void *arg) {
 jclass c=arg;jobject b=make_v(c,method(c,"<init>","()V"));jstring key=string("worker");
 jmethodID pi=method(c,"putInt","(Ljava/lang/String;I)V"),gi=method(c,"getInt","(Ljava/lang/String;)I");
 for(int i=0;i<1000;++i) {jni->CallVoidMethod(&jni,b,pi,key,i);assert(get_int_v(b,gi,key)==i);}
 jni->DeleteGlobalRef(&jni,b);return NULL;
}
int main(int argc,char**argv) {
 jni_init();vegas_bundle_install();jclass c=jni->FindClass(&jni,"android/os/Bundle");
 jclass plain=jni->FindClass(&jni,"example/Ordinary");
 assert(jni->GetStaticMethodID(&jni,(jclass)0x42424242,"ordinary","()I")==(jmethodID)1);
 jclass send=jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/SendInfo"),utils=jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/SUtils");
 jmethodID noarg=jni->GetStaticMethodID(&jni,send,"getGLUID","()[I"),witharg=jni->GetStaticMethodID(&jni,utils,"getGLUID","(Ljava/lang/String;)[I");
 assert(noarg==(jmethodID)GLUID_DEFAULT && witharg==(jmethodID)GLUID && noarg!=witharg);
 jintArray identity=jni->CallStaticObjectMethod(&jni,send,noarg);jint words[4];assert(identity && jni->GetArrayLength(&jni,identity)==4);
 jni->GetIntArrayRegion(&jni,identity,0,4,words);
 const uint32_t expected_default[]={0x8397d009,0x0445465f,0x9fb008d3,0xb4117a71};assert(!memcmp(words,expected_default,sizeof(words)));jni->DeleteGlobalRef(&jni,identity);
 identity=jni->CallStaticObjectMethod(&jni,utils,witharg,jni->NewStringUTF(&jni,"test-input"));assert(identity && jni->GetArrayLength(&jni,identity)==4);jni->GetIntArrayRegion(&jni,identity,0,4,words);
 const uint32_t expected_input[]={0x90a03ded,0x57a52a28,0x69afd1b3,0x8f6858f5};assert(!memcmp(words,expected_input,sizeof(words)));jni->DeleteGlobalRef(&jni,identity);
 jclass device=jni->FindClass(&jni,"com/gameloft/android/ANMP/GloftGGHM/GLUtils/Device");
 check_identity(device,"getHDIDFV","DE61B9E5DD0A8E44B99DF15E44D5F8F7");
 check_identity(device,"getHDIDFVVersion","v1.0.0");
 check_identity(device,"getGLDID","hdidfv=DE61B9E5DD0A8E44B99DF15E44D5F8F7 ");
 check_identity(send,"getGLDID","PSVITA-GSGV00001");

 assert(jni->CallIntMethod(&jni,NULL,method(plain,"ordinary","()I"))==777);
 assert(jni->GetStaticMethodID(&jni,jni->FindClass(&jni,"example/One"),"sameName","()Ljava/lang/String;")==(jmethodID)2);
 assert(jni->GetStaticMethodID(&jni,jni->FindClass(&jni,"example/One"),"sameName","(Ljava/lang/String;)Ljava/lang/String;")==(jmethodID)4);
 assert(jni->GetStaticMethodID(&jni,jni->FindClass(&jni,"example/Two"),"sameName","()Ljava/lang/String;")==(jmethodID)3);
 assert(!jni->GetMethodID(&jni,c,"getInt","(Ljava/lang/String;)J"));
 jmethodID ctor=method(c,"<init>","()V"); jobject a=jni->NewObjectA(&jni,c,ctor,NULL), b=jni->NewObject(&jni,c,ctor);
 jmethodID pi=method(c,"putInt","(Ljava/lang/String;I)V"),gi=method(c,"getInt","(Ljava/lang/String;)I");
 jstring k=string("key");jni->CallVoidMethod(&jni,a,pi,k,123);jni->CallVoidMethod(&jni,b,pi,k,456);
 assert(get_int_v(a,gi,k)==123 && jni->CallIntMethod(&jni,b,gi,k)==456);
 jvalue args[2]={{.l=k},{.i=-123}};jni->CallVoidMethodA(&jni,a,pi,args);assert(jni->CallIntMethodA(&jni,a,gi,args)==-123);
 jmethodID pl=method(c,"putLong","(Ljava/lang/String;J)V"),gl=method(c,"getLong","(Ljava/lang/String;)J");
 jlong wide=INT64_C(0x1234567887654321);put_long_v(a,pl,k,wide);assert(jni->CallLongMethod(&jni,a,gl,k)==wide);assert(jni->CallIntMethod(&jni,a,gi,k)==0);
 jmethodID ps=method(c,"putString","(Ljava/lang/String;Ljava/lang/String;)V"),gs=method(c,"getString","(Ljava/lang/String;)Ljava/lang/String;");
 jni->CallVoidMethod(&jni,a,ps,k,string("hello"));jstring got=jni->CallObjectMethod(&jni,a,gs,k);
 const char*value=jni->GetStringUTFChars(&jni,got,NULL);assert(!strcmp(value,"hello"));jni->ReleaseStringUTFChars(&jni,got,(char*)value);
 jni->NewGlobalRef(&jni,got);jni->DeleteLocalRef(&jni,got);value=jni->GetStringUTFChars(&jni,got,NULL);assert(!strcmp(value,"hello"));jni->ReleaseStringUTFChars(&jni,got,(char*)value);jni->DeleteGlobalRef(&jni,got);
 jmethodID pb=method(c,"putByteArray","(Ljava/lang/String;[B)V"),gb=method(c,"getByteArray","(Ljava/lang/String;)[B");jbyteArray ba=jni->NewByteArray(&jni,3);jbyte raw[3]={1,-2,3};jni->SetByteArrayRegion(&jni,ba,0,3,raw);jni->CallVoidMethod(&jni,a,pb,k,ba);raw[0]=9;jni->SetByteArrayRegion(&jni,ba,0,3,raw);jbyteArray copy=jni->CallObjectMethod(&jni,a,gb,k);jni->GetByteArrayRegion(&jni,copy,0,3,raw);assert(raw[0]==1&&raw[1]==-2&&raw[2]==3);jni->DeleteGlobalRef(&jni,copy);jni->DeleteGlobalRef(&jni,ba);
 jmethodID contains=method(c,"containsKey","(Ljava/lang/String;)Z"),clear=method(c,"clear","()V");assert(jni->CallBooleanMethod(&jni,a,contains,k));jvalue key_arg[1]={{.l=k}};
 assert(bool_v(a,contains,k)&&jni->CallBooleanMethodA(&jni,a,contains,key_arg));
 jni->CallVoidMethodA(&jni,a,clear,NULL);assert(!jni->CallBooleanMethod(&jni,a,contains,k));
 jni->CallVoidMethod(&jni,a,ps,k,string("V/A string"));
 jobject vstr=object_v(a,gs,k),astr=jni->CallObjectMethodA(&jni,a,gs,key_arg);
 assert(vstr && astr && vstr!=astr);jni->DeleteLocalRef(&jni,vstr);jni->DeleteGlobalRef(&jni,astr);
 put_long_v(a,pl,k,wide);assert(long_v(a,gl,k)==wide&&jni->CallLongMethodA(&jni,a,gl,key_arg)==wide);
 jmethodID pbool=method(c,"putBoolean","(Ljava/lang/String;Z)V"),gbool=method(c,"getBoolean","(Ljava/lang/String;)Z");
 jni->CallVoidMethod(&jni,a,pbool,k,1);assert(bool_v(a,gbool,k)&&jni->CallBooleanMethodA(&jni,a,gbool,key_arg));
 void_v(a,clear);assert(!jni->CallBooleanMethod(&jni,a,contains,k));
 pthread_t t1,t2;assert(!pthread_create(&t1,NULL,thread_test,c));assert(!pthread_create(&t2,NULL,thread_test,c));pthread_join(t1,NULL);pthread_join(t2,NULL);
 const char *unicode="Вегас — 世界";
 jni->CallVoidMethod(&jni,a,ps,k,string(unicode));
 jstring unicode_result=jni->CallObjectMethod(&jni,a,gs,k);
 assert(jni->GetStringUTFLength(&jni,unicode_result)==(jsize)strlen(unicode));
 value=jni->GetStringUTFChars(&jni,unicode_result,NULL);assert(!strcmp(value,unicode));
 jni->ReleaseStringUTFChars(&jni,unicode_result,(char*)value);jni->DeleteGlobalRef(&jni,unicode_result);
 mkdir("ux0:data",0700);mkdir("ux0:data/gangstarvegas",0700);mkdir("ux0:data/gangstarvegas/files",0700);
 jstring shared_key=string("persistent-key"),shared_missing=string("missing-key");
 jstring empty=vegas_string_preference_get("DataSharingGangstar",shared_missing,NULL);
 assert(empty && jni->GetStringLength(&jni,empty)==0);jni->DeleteLocalRef(&jni,empty);
 if(argc==1) {
  vegas_string_preference_set("DataSharingGangstar",shared_key,string("Вегас 世界"));
  vegas_string_preference_set("DataSharing",shared_key,string("separate"));
  vegas_string_preference_set("DataSharingGangstar",shared_missing,string("remove me"));
  vegas_string_preference_set("DataSharingGangstar",shared_missing,NULL);
 }
 const char *spaces[]={"DataSharingGangstar","DataSharing"},*values[]={"Вегас 世界","separate"};
 for(unsigned i=0;i<2;++i) {
  jstring item=vegas_string_preference_get(spaces[i],shared_key,NULL);assert(item);
  const char *text=jni->GetStringUTFChars(&jni,item,NULL);assert(text && !strcmp(text,values[i]));
  jni->ReleaseStringUTFChars(&jni,item,(char *)text);jni->DeleteLocalRef(&jni,item);
 }
 empty=vegas_string_preference_get("DataSharingGangstar",shared_missing,NULL);assert(empty && !jni->GetStringLength(&jni,empty));jni->DeleteLocalRef(&jni,empty);
 jstring fallback=string("fallback");
 jstring got_fallback=vegas_string_preference_get("missing-space",shared_key,fallback);
 value=jni->GetStringUTFChars(&jni,got_fallback,NULL);assert(value && !strcmp(value,"fallback"));
 jni->ReleaseStringUTFChars(&jni,got_fallback,(char *)value);jni->DeleteLocalRef(&jni,got_fallback);
 jstring stored_empty=vegas_string_preference_get("DataSharingGangstar",shared_missing,fallback);
 assert(stored_empty && !jni->GetStringLength(&jni,stored_empty));jni->DeleteLocalRef(&jni,stored_empty);
 jobject req=request(c,"namespace1","same",1);jstring data=string("npData"),result=string("npResult");
 if(argc==1) {jni->CallVoidMethod(&jni,req,pl,data,wide);vegas_preference_set(req);}
 jobject returned=vegas_preference_get(req);assert(returned==req);assert(jni->CallLongMethod(&jni,req,gl,result)==wide);jni->DeleteLocalRef(&jni,returned);
 if(argc==1) {
  const char *file="ux0:data/gangstarvegas/files/preferences.bin";
  const char *backup="ux0:data/gangstarvegas/files/preferences.saved";
  assert(!rename(file,backup));assert(!mkdir(file,0700));
  jni->CallVoidMethod(&jni,req,pl,data,(jlong)-999);vegas_preference_set(req);
  vegas_preference_get(req);assert(jni->CallLongMethod(&jni,req,gl,result)==wide);
  jni->DeleteLocalRef(&jni,req);assert(!rmdir(file));assert(!rename(backup,file));
 }
 jobject int_req=request(c,"namespace1","int",0), bool_req=request(c,"namespace1","bool",2);
 if(argc==1) {
  jni->CallVoidMethod(&jni,int_req,pi,data,-7654);vegas_preference_set(int_req);
  jni->CallVoidMethod(&jni,bool_req,pbool,data,1);vegas_preference_set(bool_req);
 }
 vegas_preference_get(int_req);vegas_preference_get(bool_req);
 assert(jni->CallIntMethod(&jni,int_req,gi,result)==-7654);
 assert(jni->CallBooleanMethod(&jni,bool_req,gbool,result));
 jni->DeleteLocalRef(&jni,int_req);jni->DeleteGlobalRef(&jni,int_req);
 jni->DeleteLocalRef(&jni,bool_req);jni->DeleteGlobalRef(&jni,bool_req);
 jobject unicode_req=request(c,"настройки","имя",3);
 if(argc==1) {jni->CallVoidMethod(&jni,unicode_req,ps,data,string(unicode));vegas_preference_set(unicode_req);}
 vegas_preference_get(unicode_req);unicode_result=jni->CallObjectMethod(&jni,unicode_req,gs,result);
 assert(unicode_result);value=jni->GetStringUTFChars(&jni,unicode_result,NULL);assert(!strcmp(value,unicode));
 jni->ReleaseStringUTFChars(&jni,unicode_result,(char*)value);jni->DeleteGlobalRef(&jni,unicode_result);
 jni->DeleteLocalRef(&jni,unicode_req);jni->DeleteGlobalRef(&jni,unicode_req);
 jobject other=request(c,"namespace2","same",1);jni->CallVoidMethod(&jni,other,pl,string("npDefaultValue"),(jlong)-25);vegas_preference_get(other);assert(jni->CallLongMethod(&jni,other,gl,result)==-25);
 jobject wrong=request(c,"namespace1","same",0);jni->CallVoidMethod(&jni,wrong,pi,string("npDefaultValue"),85);vegas_preference_get(wrong);assert(jni->CallIntMethod(&jni,wrong,gi,result)==85);
 jni->DeleteGlobalRef(&jni,a);jni->DeleteGlobalRef(&jni,b);jni->DeleteGlobalRef(&jni,req);jni->DeleteLocalRef(&jni,other);jni->DeleteGlobalRef(&jni,other);jni->DeleteLocalRef(&jni,wrong);jni->DeleteGlobalRef(&jni,wrong);
 if(argc>1 && !strcmp(argv[1],"interrupt")) {
  assert(!rename("ux0:data/gangstarvegas/files/preferences.bin","ux0:data/gangstarvegas/files/preferences.bin.previous"));
  FILE *partial=fopen("ux0:data/gangstarvegas/files/preferences.bin.tmp","wb");assert(partial);
  assert(fwrite("unfinished",1,10,partial)==10);assert(!fclose(partial));
  puts("STAGED: old save moved to .previous, incomplete .tmp left, canonical absent");
 }
 printf("PASS: receiver isolation, normal/V/A calls, 64-bit longs, type mismatch, byte deep copies, reference lifetimes, two concurrent receivers, Unicode, failed-write rollback, namespace/defaults, class/signature JNI, real GLUID callbacks with/without arguments, shared values/reload/delete, persistence %s\n",argc==1?"write":"reload");return 0;
}
