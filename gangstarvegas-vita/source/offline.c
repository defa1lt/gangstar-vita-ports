#include "offline.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#ifdef __vita__
#include "runtime.h"
#include "game_profile.h"
#include "diagnostics.h"
#endif

static int unavailable(void) { errno=ENETDOWN; return -1; }
static int offline_socket(int domain,int type,int protocol) {
    (void)domain; (void)type; (void)protocol; return unavailable();
}
static int offline_address(int fd,const struct sockaddr *address,socklen_t size) {
    (void)fd; (void)address; (void)size; return unavailable();
}
static int offline_peer(int fd,struct sockaddr *address,socklen_t *size) {
    (void)fd; (void)address; (void)size; return unavailable();
}
static int offline_listen(int fd,int backlog) { (void)fd; (void)backlog; return unavailable(); }
static int offline_option(int fd,int level,int option,const void *value,socklen_t size) {
    (void)fd; (void)level; (void)option; (void)value; (void)size; return unavailable();
}
static int offline_getoption(int fd,int level,int option,void *value,socklen_t *size) {
    (void)fd; (void)level; (void)option; (void)value; (void)size; return unavailable();
}
static ssize_t offline_recv(int fd,void *buffer,size_t size,int flags) {
    (void)fd; (void)buffer; (void)size; (void)flags; return unavailable();
}
static ssize_t offline_send(int fd,const void *buffer,size_t size,int flags) {
    (void)fd; (void)buffer; (void)size; (void)flags; return unavailable();
}
static ssize_t offline_recvfrom(int fd,void *buffer,size_t size,int flags,struct sockaddr *address,socklen_t *length) {
    (void)address; (void)length; return offline_recv(fd,buffer,size,flags);
}
static ssize_t offline_sendto(int fd,const void *buffer,size_t size,int flags,const struct sockaddr *address,socklen_t length) {
    (void)address; (void)length; return offline_send(fd,buffer,size,flags);
}
static ssize_t offline_recvmsg(int fd,struct msghdr *message,int flags) {
    (void)fd; (void)message; (void)flags; return unavailable();
}
static ssize_t offline_sendmsg(int fd,const struct msghdr *message,int flags) {
    (void)fd; (void)message; (void)flags; return unavailable();
}
static int offline_resolve(const char *node,const char *service,const void *hints,void **result) {
    (void)node; (void)service; (void)hints;
    if (result) *result=NULL;
    errno=ENETDOWN;
    return 4; /* Android EAI_FAIL; newlib uses a different ABI. */
}
static void *offline_host(const char *name) { (void)name; unavailable(); return NULL; }
static void offline_free(void *result) { (void)result; }
static int offline_hostname(char *name,size_t size) {
    if (name && size) *name='\0';
    return unavailable();
}
static long offline_syscall(long number,...) { (void)number; errno=ENOSYS; return -1; }
static const struct { const char *name; uintptr_t address; } symbols[]={
    {"socket",(uintptr_t)offline_socket},
    {"connect",(uintptr_t)offline_address},{"bind",(uintptr_t)offline_address},
    {"accept",(uintptr_t)offline_peer},{"getpeername",(uintptr_t)offline_peer},{"getsockname",(uintptr_t)offline_peer},
    {"listen",(uintptr_t)offline_listen},{"shutdown",(uintptr_t)offline_listen},
    {"setsockopt",(uintptr_t)offline_option},{"getsockopt",(uintptr_t)offline_getoption},
    {"recv",(uintptr_t)offline_recv},{"send",(uintptr_t)offline_send},
    {"recvfrom",(uintptr_t)offline_recvfrom},{"sendto",(uintptr_t)offline_sendto},
    {"recvmsg",(uintptr_t)offline_recvmsg},{"sendmsg",(uintptr_t)offline_sendmsg},
    {"getaddrinfo",(uintptr_t)offline_resolve},{"freeaddrinfo",(uintptr_t)offline_free},
    {"gethostbyname",(uintptr_t)offline_host},{"gethostname",(uintptr_t)offline_hostname},
    {"syscall",(uintptr_t)offline_syscall},
};
void *vegas_offline_symbol(const char *name) {
    for (size_t i=0;i<sizeof(symbols)/sizeof(symbols[0]);++i)
        if (!strcmp(name,symbols[i].name)) return (void *)symbols[i].address;
    return NULL;
}
int vegas_offline_probe(void) {
    int saved=errno;
    void *result=(void *)1;
    int passed=offline_socket(2,1,0)==-1 && errno==ENETDOWN &&
        offline_resolve("localhost",NULL,NULL,&result)==4 && !result && errno==ENETDOWN;
    errno=saved;
    return passed?0:-1;
}

static const char *clock_empty_string="";
int vegas_offline_timestamp(void *service, int32_t *timestamp, int asynchronous,
                            VegasTimeCallback callback, void *userdata) {
    (void)service;
    time_t now=time(NULL);
    int result=timestamp && now>=0 && (uint64_t)now<=INT32_MAX ? 0 : -1;
    if (!result) *timestamp=(int32_t)now;
#ifdef __vita__
    LOG_INFO("Offline clock: RTC=%lld async=%d result=%d callback=%p",(long long)now,asynchronous,result,callback);
#endif
    if (asynchronous && callback) callback(3001,&clock_empty_string,result,userdata);
    return result;
}
#ifdef __vita__
int vegas_offline_bind_clock(void) {
    uint32_t *entry=(void *)(so_mod.text_base+GAME_OFFLINE_CLOCK_ENTRY);
    const uint32_t *rep=(void *)(so_mod.text_base+GAME_OFFLINE_CLOCK_EMPTY_REP);
    if (entry[0]!=GAME_OFFLINE_CLOCK_WORD0 || entry[1]!=GAME_OFFLINE_CLOCK_WORD1 ||
        rep[0] || rep[1] || *(const char *)(rep+3)) {
        LOG_ERROR("Offline clock profile mismatch"); return -1;
    }
    // Use the game's own GNU string representation for callback const std::string&.
    clock_empty_string=(const char *)(rep+3);
    so_hook hook=hook_arm((uintptr_t)entry,(uintptr_t)vegas_offline_timestamp);
    if (memcmp(entry,hook.patch_instr,sizeof(hook.patch_instr))) {
        LOG_ERROR("Offline clock binding failed"); return -1;
    }
    LOG_SUCCESS("Gaia timestamp provider bound to the Vita RTC");
    return 0;
}
#endif
