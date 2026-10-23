#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "md5.h"
void *mmap(void *,size_t,int,int,int,off_t);
int munmap(void *,size_t);
int vegas_msync(void *,size_t,int);
typedef struct { uint32_t events,padding; uint64_t data; } Event;
int vegas_epoll_create(int),vegas_epoll_ctl(int,int,int,Event *),vegas_epoll_wait(int,Event *,int,int),vegas_close(int);
ssize_t vegas_read(int,void *,size_t);
static void mapping_test(void) {
    char name[]="/tmp/vegas-mapping-XXXXXX";int fd=mkstemp(name);assert(fd>=0);unlink(name);
    char data[8192];memset(data,'a',4096);memset(data+4096,'b',4096);assert(write(fd,data,sizeof(data))==sizeof(data));
    assert(lseek(fd,13,SEEK_SET)==13);
    char *p=mmap(NULL,4096,3,1,fd,4096);assert(p!=(void *)-1 && !((uintptr_t)p&4095));assert(p[0]=='b');assert(lseek(fd,0,SEEK_CUR)==13);
    p[42]='c';assert(vegas_msync(p,4096,2)==0);char c;assert(pread(fd,&c,1,4138)==1 && c=='c');assert(munmap(p,4096)==0);
    p=mmap(NULL,4096,3,2,fd,0);assert(p!=(void *)-1);p[0]='z';assert(vegas_msync(p,4096,4)==0);assert(munmap(p,4096)==0);assert(pread(fd,&c,1,0)==1 && c=='a');
    p=mmap(NULL,17,3,0x22,-1,0);assert(p!=(void *)-1 && p[0]==0);assert(munmap(p,16)==-1);assert(munmap(p,17)==0);close(fd);
}
static void epoll_test(void) {
    int socket[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,socket)==0);
    assert(fcntl(socket[0],F_SETFL,O_NONBLOCK)==0);
    int ep=vegas_epoll_create(1);assert(ep>=0);Event event={0x80000001u,0,UINT64_C(0x123456789abcdef)},out;
    assert(vegas_epoll_ctl(ep,1,socket[0],&event)==0);assert(vegas_epoll_ctl(ep,1,socket[0],&event)==-1 && errno==EEXIST);
    assert(write(socket[1],"a",1)==1);assert(vegas_epoll_wait(ep,&out,1,100)==1 && out.data==event.data && (out.events&1));assert(vegas_epoll_wait(ep,&out,1,0)==0);
    char c;assert(vegas_read(socket[0],&c,1)==1 && c=='a');assert(vegas_read(socket[0],&c,1)==-1 && errno==EAGAIN);
    assert(write(socket[1],"b",1)==1);assert(vegas_epoll_wait(ep,&out,1,100)==1);
    event.events=0x40000001u;assert(vegas_epoll_ctl(ep,3,socket[0],&event)==0);assert(vegas_epoll_wait(ep,&out,1,0)==1);assert(vegas_epoll_wait(ep,&out,1,0)==0);
    assert(vegas_epoll_ctl(ep,3,socket[0],&event)==0);assert(vegas_epoll_wait(ep,&out,1,0)==1);
    assert(vegas_close(socket[0])==0);close(socket[1]);assert(vegas_epoll_wait(ep,&out,1,2)==0);assert(vegas_close(ep)==0);
    int pipefd[2];assert(pipe(pipefd)==0);ep=vegas_epoll_create(1);event.events=1;
    assert(vegas_epoll_ctl(ep,1,pipefd[0],&event)==0);assert(write(pipefd[1],"p",1)==1);assert(vegas_epoll_wait(ep,&out,1,100)==1);assert(vegas_read(pipefd[0],&c,1)==1);assert(vegas_epoll_wait(ep,&out,1,0)==0);
    assert(vegas_close(pipefd[0])==0);close(pipefd[1]);assert(vegas_close(ep)==0);
}
int main(void) {
    uint8_t digest[16];vegas_md5("abc",3,digest);const uint8_t expected[16]={0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};assert(!memcmp(digest,expected,16));
    char hex[33]; vegas_md5_hex("abc",3,hex); assert(!strcmp(hex,"900150983CD24FB0D6963F7D28E17F72"));
    mapping_test();epoll_test();puts("PASS: MD5 vector, file mappings, shared/private sync, sockets/pipes, edge rearm and one-shot");
}
