#include "offline.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#define BIND(type,name) type=(void *)vegas_offline_symbol(name); assert(vegas_offline_symbol(name))
#define DENIED(call) do { errno=0; assert((call)==-1 && errno==ENETDOWN); } while (0)
static unsigned clock_callbacks;
static int clock_result;
static void clock_complete(int opcode,const void *message,int result,void *userdata) {
    assert(opcode==3001 && userdata==&clock_callbacks);
    assert(message && !(*(const char *const *)message)[0]);
    ++clock_callbacks;
    clock_result=result;
}
int main(void) {
    BIND(int (*create)(int,int,int),"socket");
    DENIED(create(AF_INET,SOCK_STREAM,0)); DENIED(create(AF_INET6,SOCK_DGRAM,0));
    DENIED(create(AF_UNIX,SOCK_STREAM,0));
    BIND(int (*resolve)(const char *,const char *,const void *,void **),"getaddrinfo");
    void *result=(void *)1; assert(resolve("127.0.0.1","80",NULL,&result)==4 && !result && errno==ENETDOWN);
    BIND(void *(*host)(const char *),"gethostbyname"); assert(!host("localhost") && errno==ENETDOWN);
    BIND(int (*hostname)(char *,size_t),"gethostname"); char output[16]="unchanged";
    DENIED(hostname(output,sizeof(output))); assert(!output[0]);
    BIND(void (*release)(void *),"freeaddrinfo"); release(NULL);
    BIND(long (*system_call)(long,...),"syscall"); assert(system_call(281,2,1,0)==-1 && errno==ENOSYS);
    int fds[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
    BIND(ssize_t (*transmit)(int,const void *,size_t,int),"send"); DENIED(transmit(fds[0],"x",1,0));
    assert(write(fds[1],"a",1)==1);
    BIND(ssize_t (*receive)(int,void *,size_t,int),"recv"); char c='z'; DENIED(receive(fds[0],&c,1,0)); assert(c=='z');
    assert(read(fds[0],&c,1)==1 && c=='a');
    BIND(ssize_t (*transmitto)(int,const void *,size_t,int,const struct sockaddr *,socklen_t),"sendto");
    DENIED(transmitto(fds[0],"x",1,0,NULL,0));
    BIND(ssize_t (*receivefrom)(int,void *,size_t,int,struct sockaddr *,socklen_t *),"recvfrom");
    socklen_t size=19; DENIED(receivefrom(fds[0],&c,1,0,NULL,&size)); assert(size==19);
    BIND(ssize_t (*transmitmsg)(int,const struct msghdr *,int),"sendmsg"); DENIED(transmitmsg(fds[0],NULL,0));
    BIND(ssize_t (*receivemsg)(int,struct msghdr *,int),"recvmsg"); DENIED(receivemsg(fds[0],NULL,0));
    const char *address_names[]={"connect","bind"};
    for (unsigned i=0;i<2;++i) { BIND(int (*address)(int,const struct sockaddr *,socklen_t),address_names[i]); DENIED(address(fds[0],NULL,0)); }
    const char *peer_names[]={"accept","getpeername","getsockname"};
    for (unsigned i=0;i<3;++i) { BIND(int (*peer)(int,struct sockaddr *,socklen_t *),peer_names[i]); DENIED(peer(fds[0],NULL,&size)); assert(size==19); }
    const char *integer_names[]={"listen","shutdown"};
    for (unsigned i=0;i<2;++i) { BIND(int (*integer)(int,int),integer_names[i]); DENIED(integer(fds[0],0)); }
    BIND(int (*setoption)(int,int,int,const void *,socklen_t),"setsockopt"); DENIED(setoption(fds[0],0,0,NULL,0));
    BIND(int (*getoption)(int,int,int,void *,socklen_t *),"getsockopt"); DENIED(getoption(fds[0],0,0,NULL,&size)); assert(size==19);
    assert(vegas_offline_probe()==0); assert(!vegas_offline_symbol("read")); assert(!vegas_offline_symbol("pipe"));
    int32_t timestamp=0;
    time_t before=time(NULL);
    assert(vegas_offline_timestamp(NULL,&timestamp,0,clock_complete,&clock_callbacks)==0);
    assert(timestamp>=before && timestamp<=time(NULL) && clock_callbacks==0);
    assert(vegas_offline_timestamp(NULL,&timestamp,1,clock_complete,&clock_callbacks)==0);
    assert(clock_callbacks==1 && clock_result==0 && timestamp>=before && timestamp<=time(NULL));
    assert(vegas_offline_timestamp(NULL,NULL,1,clock_complete,&clock_callbacks)==-1);
    assert(clock_callbacks==2 && clock_result!=0);
    close(fds[0]);close(fds[1]);
    puts("PASS: all 20 imported network operations disabled, DNS outputs cleared, raw syscall disabled, local I/O preserved");
}
