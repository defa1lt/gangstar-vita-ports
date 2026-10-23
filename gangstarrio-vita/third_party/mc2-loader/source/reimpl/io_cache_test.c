#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "../utils/texture_mip.h"

typedef int SceUID;
typedef size_t SceSize;
typedef off_t SceOff;
typedef struct { uint32_t size; const char *name; } ObbEntry;
static _Thread_local struct {
    unsigned cache_lock_calls, cache_lock_us;
} s_physical_stats;

static uint64_t sceKernelGetSystemTimeWide(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void port_trace(const char *format, ...) { (void)format; }
static int profiled_pread(SceUID fd, void *data, SceSize size, SceOff offset);
#include "cache_under_test.h"

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static int paused_fd = -1, read_entered, resume_read;
static unsigned reads;
static size_t last_read_size;
static off_t last_read_offset;

static int profiled_pread(SceUID fd, void *data, SceSize size, SceOff offset) {
    /* Exercise the real reader; the gate only fixes the order of the threads. */
    assert(pthread_mutex_trylock(&s_texture_cache_lock) == 0);
    pthread_mutex_unlock(&s_texture_cache_lock);
    pthread_mutex_lock(&gate_lock);
    ++reads;
    last_read_size = size;
    last_read_offset = offset;
    if (fd == paused_fd) {
        read_entered = 1;
        pthread_cond_broadcast(&gate);
        while (!resume_read) pthread_cond_wait(&gate, &gate_lock);
    }
    pthread_mutex_unlock(&gate_lock);
    return (int)pread(fd, data, size, offset);
}

static int make_file(unsigned size, unsigned char value) {
    char path[] = "/tmp/rio-cache-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(unlink(path) == 0);
    unsigned char block[16384];
    memset(block, value, sizeof(block));
    for (unsigned done = 0; done < size;) {
        unsigned chunk = size - done;
        if (chunk > sizeof(block)) chunk = sizeof(block);
        assert(write(fd, block, chunk) == (ssize_t)chunk);
        done += chunk;
    }
    return fd;
}

static void clear_cache(void) {
    for (unsigned i = 0; i < TEXTURE_CACHE_SLOTS; ++i) {
        assert(!s_texture_cache[i].loading);
        free(s_texture_cache[i].data);
    }
    memset(s_texture_cache, 0, sizeof(s_texture_cache));
    s_texture_cache_bytes = 0;
}

static void expect_bytes(const unsigned char *bytes, unsigned size, unsigned char value) {
    for (unsigned i = 0; i < size; ++i) assert(bytes[i] == value);
}

typedef struct {
    ObbEntry entry;
    int fd;
    unsigned char value;
    unsigned offset, size;
} ReadJob;

static void *fill(void *argument) {
    ReadJob *job = argument;
    unsigned char *output = malloc(job->size);
    assert(output);
    assert(read_texture_cache(&job->entry, job->fd, 0, job->offset,
                                    job->size, output, 1) == 1);
    expect_bytes(output, job->size, job->value);
    free(output);
    return NULL;
}

static void check_dds_tail(void) {
    uint32_t header[32] = {0x20534444u, 124, 0x21007, 2048, 2048, 0, 0, 12};
    header[19] = 32;
    header[20] = 4;
    header[21] = 0x30435445u;
    ObbEntry entry = {2796344, "_packEtc/mip-chain.tga"};
    int fd = make_file(entry.size, 0xa5);
    assert(pwrite(fd, header, sizeof(header), 0) == sizeof(header));
    unsigned w = header[4], h = header[3];
    unsigned selected = texture_select_dds_dimensions(&w, &h, header[7], 512, header[2]);
    assert(selected == 2 && w == 512 && h == 512);
    unsigned offset = sizeof(header), source_w = header[4], source_h = header[3];
    for (unsigned level = 0; level < selected; ++level) {
        offset += source_w * source_h / 2;
        source_w >>= 1;
        source_h >>= 1;
    }
    unsigned size = entry.size - offset;
    assert(offset == 2621568 && size == 174776);
    unsigned char *output = malloc(size);
    assert(output);
    memset(output, 0x5a, size);
    assert(pwrite(fd, output, size, offset) == size);
    unsigned before = reads;
    assert(read_texture_cache(&entry, fd, 0, offset, size, output, 1));
    assert(reads == before + 1 && last_read_offset == offset && last_read_size == size);
    assert(s_texture_cache_bytes == size);
    expect_bytes(output, size, 0x5a);
    unsigned char discarded;
    assert(pread(fd, &discarded, 1, sizeof(header)) == 1 && discarded == 0xa5);
    assert(ftruncate(fd, 0) == 0);
    assert(read_texture_cache(&entry, fd, 0, offset + 1024, size - 1024, output, 1));
    assert(reads == before + 1);
    expect_bytes(output, size - 1024, 0x5a);
    free(output);
    close(fd);
    clear_cache();
}

typedef struct {
    ObbEntry entry;
    int fd;
    unsigned base, payload_offset, payload_size;
} PrefetchJob;

static void *prefetch(void *argument) {
    PrefetchJob *job = argument;
    uint32_t header[256];
    assert(read_texture_cache(&job->entry, job->fd, job->base, 0,
                              sizeof(header), header, 1));
    assert(texture_dds_payload_range(header, job->entry.size, 512,
                                     &job->payload_offset, &job->payload_size));
    assert(read_texture_cache(&job->entry, job->fd, job->base,
                              job->payload_offset, job->payload_size, NULL, 1));
    return NULL;
}

static void check_worker_prefetch(unsigned archive_prefix, const uint32_t header[32]) {
    PrefetchJob job = {{2796344, "_packEtc/archive-texture.bmp"}, -1,
                      archive_prefix, 0, 0};
    job.fd = make_file(archive_prefix + job.entry.size, 0x73);
    assert(pwrite(job.fd, header, 128, archive_prefix) == 128);
    unsigned before = reads;
    unsigned char output[1024];
    memset(output, 0x29, sizeof(output));
    assert(!read_texture_cache(&job.entry, job.fd, archive_prefix, 0,
                               sizeof(output), output, 0));
    assert(reads == before && s_texture_cache_bytes == 0);
    expect_bytes(output, sizeof(output), 0x29);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, prefetch, &job) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(job.payload_offset == 2621568 && job.payload_size == 174776);
    assert(reads == before + 2 && last_read_offset == archive_prefix + job.payload_offset);
    assert(s_texture_cache_bytes == sizeof(output) + job.payload_size);
    /* The consumer must work without physical storage after worker completion. */
    assert(ftruncate(job.fd, 0) == 0);
    assert(read_texture_cache(&job.entry, -1, 0, 0, sizeof(output), output, 0));
    assert(memcmp(output, header, 128) == 0);
    assert(read_texture_cache(&job.entry, -1, 0, job.payload_offset,
                              sizeof(output), output, 0));
    expect_bytes(output, sizeof(output), 0x73);
    assert(read_texture_cache(&job.entry, -1, 0,
                              job.entry.size - sizeof(output), sizeof(output), output, 0));
    expect_bytes(output, sizeof(output), 0x73);
    assert(!read_texture_cache(&job.entry, -1, 0, 4096, sizeof(output), output, 0));
    assert(reads == before + 2);
    close(job.fd); clear_cache();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    uint32_t fixture_header[32];
    FILE *fixture = fopen(argv[1], "rb");
    assert(fixture && fread(fixture_header, 1, sizeof(fixture_header), fixture) == sizeof(fixture_header));
    assert(fclose(fixture) == 0);
    const unsigned mib = 1024 * 1024;
    unsigned char output[65536];
    ObbEntry entry = {2 * mib, "_packEtc/test.tga"};
    int fd = make_file(entry.size, 0x37);
    unsigned char marker = 0x92;
    assert(pwrite(fd, &marker, 1, 37) == 1);
    assert(read_texture_cache(&entry, fd, 0, 1024, sizeof(output), output, 1));
    expect_bytes(output, sizeof(output), 0x37);
    assert(last_read_offset == 1024 && last_read_size == sizeof(output));
    assert(s_texture_cache_bytes == sizeof(output));
    assert(read_texture_cache(&entry, fd, 0, 37, sizeof(output), output, 1));
    assert(output[0] == marker);
    expect_bytes(output + 1, sizeof(output) - 1, 0x37);
    assert(last_read_offset == 37 && last_read_size == sizeof(output));
    unsigned before = reads;
    assert(ftruncate(fd, 0) == 0);
    assert(read_texture_cache(&entry, fd, 0, 2048, sizeof(output) / 2, output, 1));
    assert(reads == before);
    expect_bytes(output, sizeof(output) / 2, 0x37);
    assert(read_texture_cache(&entry, fd, 0, 37, sizeof(output), output, 1));
    assert(output[0] == marker && reads == before);
    assert(!read_texture_cache(&entry, fd, 0, entry.size - 1, 2, output, 1));
    clear_cache();
    assert(!read_texture_cache(&entry, fd, 0, 0, sizeof(output), output, 1));
    assert(s_texture_cache_bytes == 0);
    close(fd);

    ObbEntry hot = {mib / 2, "_packEtc/hot.tga"};
    int hot_fd = make_file(hot.size, 0x49);
    assert(read_texture_cache(&hot, hot_fd, 0, 0, sizeof(output), output, 1));
    ReadJob job = {{12 * mib + 1024, "_packEtc/slow.tga"}, -1, 0x75,
                   1024, 12 * mib};
    job.fd = make_file(job.entry.size, job.value);
    pthread_mutex_lock(&gate_lock);
    paused_fd = job.fd;
    read_entered = resume_read = 0;
    pthread_mutex_unlock(&gate_lock);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fill, &job) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!read_entered) pthread_cond_wait(&gate, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    assert(read_texture_cache(&hot, hot_fd, 0, 0, sizeof(output), output, 1));
    expect_bytes(output, sizeof(output), 0x49);
    memset(output, 0x29, sizeof(output));
    assert(!read_texture_cache(&job.entry, job.fd, 0, job.offset, sizeof(output), output, 1));
    expect_bytes(output, sizeof(output), 0x29);
    ObbEntry other = {8 * mib, "_packEtc/other.tga"};
    int other_fd = make_file(other.size, 0x63);
    unsigned char *large_output = malloc(other.size);
    assert(large_output);
    before = reads;
    assert(!read_texture_cache(&other, other_fd, 0, 0, other.size, large_output, 1));
    assert(reads == before && s_texture_cache_bytes == job.size);

    pthread_mutex_lock(&gate_lock);
    resume_read = 1;
    pthread_cond_broadcast(&gate);
    pthread_mutex_unlock(&gate_lock);
    assert(pthread_join(thread, NULL) == 0);
    assert(read_texture_cache(&other, other_fd, 0, 0, other.size, large_output, 1));
    expect_bytes(large_output, other.size, 0x63);
    assert(s_texture_cache_bytes == other.size);
    free(large_output);
    close(hot_fd); close(job.fd); close(other_fd);
    clear_cache();

    check_dds_tail();
    check_worker_prefetch(0, fixture_header);
    check_worker_prefetch(4096, fixture_header);
    puts("streaming cache: real range reads, overlapping windows, hits, bounds, short reads, concurrent fill and eviction passed");
    return 0;
}
