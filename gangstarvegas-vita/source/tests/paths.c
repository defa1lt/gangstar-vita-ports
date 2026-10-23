#include "paths-under-test.c"
#include <assert.h>
#include <unistd.h>
static void check(const char *input,const char *expected) {
    char buffer[PATH_MAX]; const char *actual=translate_bundle_path(input,buffer);
    assert(actual && !strcmp(actual,expected));
}
int main(void) {
    check(DATA_PATH "files//" DATA_PATH "main.17026.com.gameloft.android.anmp.gloftgghm.obb.indexfile",
          DATA_PATH "files/main.17026.com.gameloft.android.anmp.gloftgghm.obb.indexfile");
    check(DATA_PATH "files/" DATA_PATH DATA_PATH "patch.17026.com.gameloft.android.anmp.gloftgghm.obb.indexfile",
          DATA_PATH "files/patch.17026.com.gameloft.android.anmp.gloftgghm.obb.indexfile");
    check(DATA_PATH "files//xmldata.zip.indexfile",DATA_PATH "files/xmldata.zip.indexfile");
    check(DATA_PATH "files/avatars/player/image.png",DATA_PATH "files/avatars/player/image.png");
    check(DATA_PATH "files/avatars/" DATA_PATH "image.png",DATA_PATH "files/avatars/" DATA_PATH "image.png");
    check(DATA_PATH "main.17026.com.gameloft.android.ANMP.GloftGGHM.obb",DATA_PATH "main.17026.com.gameloft.android.ANMP.GloftGGHM.obb");
    check("/data/data/com.gameloft.android.ANMP.GloftGGHM/files/save",DATA_PATH "files/save");
    check("app0:/cpuinfo","app0:/cpuinfo");
    char *expanded=expand_template("SOURCEGAMECODE MARKET GAMEVERSIONGC_LETTER LANGCODE COUNTRYOFDEVICE DEVICE_NAME FIRMWARE UDID HDIDFV ANDROID_ID");
    assert(expanded && !strcmp(expanded,"GGHM ANMP 1.7.0g EN US Sony_PlayStationVita PlayStation Vita PSVITA-GSGV00001 DE61B9E5DD0A8E44B99DF15E44D5F8F7 PSVITA-GSGV00001"));free(expanded);
    expanded=expand_template("unchanged 世界");assert(expanded && !strcmp(expanded,"unchanged 世界"));free(expanded);
    assert(!expand_template(NULL));
    int fds[2];assert(!pipe(fds));
    assert(!fcntl_soloader(fds[0],4,04000));assert(fcntl_soloader(fds[0],3)&04000);
    char c;assert(read(fds[0],&c,1)==-1 && errno==EAGAIN);
    assert(!fcntl_soloader(fds[0],2,1));assert(fcntl_soloader(fds[0],1)&FD_CLOEXEC);
    int copy=fcntl_soloader(fds[0],0,0);assert(copy>=0 && copy!=fds[0]);
    assert(fcntl_soloader(copy,999)==-1 && errno==EINVAL);
    close(copy);close(fds[0]);close(fds[1]);
    puts("PASS: production path translator preserves archive index paths and distinct save hierarchies; Android fcntl nonblocking/CLOEXEC/dup/error; local template expansion");
}
