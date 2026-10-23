#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { uint32_t events; uint32_t padding; uint64_t data; } AndroidEvent;
_Static_assert(sizeof(AndroidEvent) == 16, "ARM epoll_event ABI");
typedef struct { int fd; AndroidEvent event; unsigned last_ready; int armed; } Watch;
typedef struct PollSet { int id; Watch *watch; size_t count; struct PollSet *next; } PollSet;
static PollSet *sets;
static int next_id = 0x60000000;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PollSet *find(int id) { for (PollSet *p = sets; p; p = p->next) if (p->id == id) return p; return NULL; }
static int64_t milliseconds(void) { struct timeval now; gettimeofday(&now, NULL); return (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000; }
int vegas_epoll_create(int size) {
    if (size <= 0) { errno = EINVAL; return -1; }
    PollSet *p = calloc(1, sizeof(*p)); if (!p) return -1;
    pthread_mutex_lock(&lock); p->id = next_id++; p->next = sets; sets = p; pthread_mutex_unlock(&lock);
    return p->id;
}
int vegas_epoll_ctl(int id, int operation, int fd, AndroidEvent *event) {
    pthread_mutex_lock(&lock);
    PollSet *p = find(id);
    if (!p) { errno = EBADF; goto fail; }
    size_t index = 0; while (index < p->count && p->watch[index].fd != fd) ++index;
    if (operation == 1 && index < p->count) { errno = EEXIST; goto fail; }
    if (operation != 1 && index == p->count) { errno = ENOENT; goto fail; }
    if (operation == 2) { p->watch[index] = p->watch[--p->count]; pthread_mutex_unlock(&lock); return 0; }
    if ((operation != 1 && operation != 3) || !event || fd < 0 || fd == id) { errno = EINVAL; goto fail; }
    if (event->events & ~(0xc0000000u | 0x201fu)) { errno = EINVAL; goto fail; }
    if (operation == 1) {
        Watch *grown = realloc(p->watch, (p->count + 1) * sizeof(*grown));
        if (!grown) goto fail;
        p->watch = grown; ++p->count;
    }
    p->watch[index] = (Watch){fd, *event, 0, 1};
    pthread_mutex_unlock(&lock); return 0;
fail:
    pthread_mutex_unlock(&lock); return -1;
}
int vegas_epoll_wait(int id, AndroidEvent *events, int capacity, int timeout) {
    if (!events || capacity <= 0) { errno = EINVAL; return -1; }
    int64_t deadline = timeout < 0 ? INT64_MAX : milliseconds() + timeout;
    for (;;) {
        pthread_mutex_lock(&lock); PollSet *p = find(id);
        if (!p) { pthread_mutex_unlock(&lock); errno = EBADF; return -1; }
        size_t count = p->count;
        struct pollfd *fds = calloc(count ? count : 1, sizeof(*fds));
        if (!fds) { pthread_mutex_unlock(&lock); return -1; }
        for (size_t i = 0; i < count; ++i) {
            fds[i].fd = p->watch[i].armed ? p->watch[i].fd : -1;
            fds[i].events = p->watch[i].event.events & 0x1f;
        }
        pthread_mutex_unlock(&lock);
        int64_t remaining = deadline - milliseconds();
        int wait_ms = timeout == 0 || remaining <= 0 ? 0 : remaining > 20 ? 20 : (int)remaining;
        int ready = poll(fds, count, wait_ms);
        if (ready < 0) { free(fds); return -1; }
        int emitted = 0;
        pthread_mutex_lock(&lock); p = find(id);
        if (!p) { pthread_mutex_unlock(&lock); free(fds); errno = EBADF; return -1; }
        for (size_t i = 0; i < count && emitted < capacity; ++i) {
            for (size_t j = 0; j < p->count; ++j) {
                Watch *watch = &p->watch[j]; if (watch->fd != fds[i].fd || !watch->armed) continue;
                unsigned current = (unsigned)fds[i].revents & 0x1f;
                if (fds[i].revents & POLLNVAL) current |= 8;
                unsigned notify = watch->event.events & 0x80000000u ? current & ~watch->last_ready : current;
                watch->last_ready = current;
                if (notify) {
                    events[emitted] = watch->event; events[emitted++].events = notify;
                    if (watch->event.events & 0x40000000u) watch->armed = 0;
                }
                break;
            }
        }
        pthread_mutex_unlock(&lock); free(fds);
        if (emitted || milliseconds() >= deadline || timeout == 0) return emitted;
        if (ready) usleep(1000);
    }
}
static void consumed(int fd, unsigned drained) {
    int saved_errno = errno;
    struct pollfd check = {fd, POLLIN | POLLOUT, 0};
    if (poll(&check, 1, 0) < 0) { errno = saved_errno; return; }
    pthread_mutex_lock(&lock);
    for (PollSet *p = sets; p; p = p->next)
        for (size_t i = 0; i < p->count; ++i)
            if (p->watch[i].fd == fd) p->watch[i].last_ready &= (unsigned)check.revents & ~drained;
    pthread_mutex_unlock(&lock);
    errno = saved_errno;
}
ssize_t vegas_read(int fd, void *buffer, size_t length) { ssize_t n = read(fd, buffer, length); consumed(fd,n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)?1u:0); return n; }
ssize_t vegas_write(int fd, const void *buffer, size_t length) { ssize_t n = write(fd, buffer, length); consumed(fd,n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)?4u:0); return n; }
int vegas_close(int fd) {
    pthread_mutex_lock(&lock); PollSet **link = &sets;
    while (*link && (*link)->id != fd) link = &(*link)->next;
    if (*link) { PollSet *p = *link; *link = p->next; free(p->watch); free(p); pthread_mutex_unlock(&lock); return 0; }
    for (PollSet *p = sets; p; p = p->next)
        for (size_t i = 0; i < p->count; )
            if (p->watch[i].fd == fd) p->watch[i] = p->watch[--p->count]; else ++i;
    pthread_mutex_unlock(&lock); return close(fd);
}
