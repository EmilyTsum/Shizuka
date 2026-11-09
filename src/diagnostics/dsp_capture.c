// SPDX-License-Identifier: GPL-2.0-only
// Opt-in, bounded stock FARF capture. Never writes a DMA ring, audio parameter or property.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define RING_SIZE 262144
#define CAPACITY (RING_SIZE - 4)
#define MAX_MESSAGE 8192
#define WATCHED "audioadsprpcd.farf"
static const char farf_config[] = "0x00000004 aptx-adaptive-param-r3.c,aptx-adadptive-ctl-r3.c,capi_aptx_adaptive_enc_utils.cpp,capi_aptx_adaptive_encoder.c,capi_aptx_adaptive_encoder_r3.c,capi_aptx_adaptive_encoder_wrappers.c\n";
static volatile sig_atomic_t stopped;
static void stop_capture(int signal_number) { (void)signal_number; stopped = 1; }
static double monotonic(void) {
    struct timespec time; clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}
static void pause_poll(void) { struct timespec time = {.tv_nsec = 20000000}; nanosleep(&time, NULL); }
static int identical(const struct stat *left, const struct stat *right) {
    return S_ISREG(right->st_mode) && left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}
static int unlink_owned(int directory, const char *name, const struct stat *identity) {
    struct stat actual;
    if (fstatat(directory, name, &actual, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -1;
    if (!identical(identity, &actual)) { errno = ESTALE; return -1; }
    return unlinkat(directory, name, 0);
}
static void quoted(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
typedef struct { char text[MAX_MESSAGE]; size_t used; int skip; unsigned records, discarded; } Decoder;
static void consume(Decoder *decoder, unsigned char byte, double time) {
    if (!byte) {
        if (decoder->skip) decoder->skip = 0;
        else if (decoder->used) {
            decoder->text[decoder->used] = 0;
            printf("{\"type\":\"dsp_log\",\"monotonic_s\":%.6f,\"text\":", time);
            quoted(decoder->text); puts("}"); decoder->records++;
        }
        decoder->used = 0;
    } else if (!decoder->skip) {
        if ((byte >= 32 && byte < 127) || byte == '\n' || byte == '\t') {
            if (decoder->used < sizeof(decoder->text) - 1) decoder->text[decoder->used++] = (char)byte;
            else { decoder->used = 0; decoder->skip = 1; decoder->discarded++; }
        } else { decoder->used = 0; decoder->skip = 1; decoder->discarded++; }
    }
}

static unsigned long long process_start(int pid) {
    char path[64], text[4096]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t size = read(fd, text, sizeof(text)-1); close(fd); if (size <= 0) return 0;
    text[size] = 0; char *end = strrchr(text, ')'); if (!end || end[1] != ' ') return 0;
    char *field = end + 2;
    for (int i = 0; i < 19; ++i) { field = strchr(field, ' '); if (!field) return 0; field++; }
    return strtoull(field, NULL, 10);
}
static int boot_id(char *out, size_t capacity) {
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t size = read(fd, out, capacity-1); close(fd); if (size < 36) return -1;
    out[36] = 0; return 0;
}
static int recover_directory(DIR *directory, const char *boot, uid_t expected_owner) {
    int fd = dirfd(directory), result = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        int pid; unsigned long long ticks; char previous_boot[37], tail;
        if (sscanf(entry->d_name, ".shizuka-diagnostics-%d-%llu-%36[0-9a-f-].tmp%c", &pid, &ticks, previous_boot, &tail) != 3) continue;
        char canonical[128]; snprintf(canonical, sizeof(canonical), ".shizuka-diagnostics-%d-%llu-%s.tmp", pid, ticks, previous_boot);
        if (pid < 2 || !ticks || strlen(previous_boot) != 36 || strcmp(canonical, entry->d_name)) continue;
        if (!strcmp(boot, previous_boot) && process_start(pid) == ticks) continue;
        int file = openat(fd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (file < 0) { result = 76; continue; }
        struct stat identity; char content[sizeof(farf_config)];
        int owned = !fstat(file, &identity) && S_ISREG(identity.st_mode) && identity.st_uid == expected_owner &&
            (identity.st_mode & 0777) == 0600 && identity.st_size == sizeof(farf_config)-1 &&
            read(file, content, sizeof(content)) == sizeof(farf_config)-1 && !memcmp(content, farf_config, sizeof(farf_config)-1);
        close(file); if (!owned) { result = 76; continue; }
        int watched = unlink_owned(fd, WATCHED, &identity);
        int stage = unlink_owned(fd, entry->d_name, &identity);
        if (watched || stage) result = 76;
    }
    return result;
}
static int recover(void) {
    char boot[64]; if (boot_id(boot, sizeof(boot))) return 76;
    DIR *directory = opendir("/data/vendor/audio_dsp");
    if (!directory) return errno == ENOENT ? 0 : 76;
    int result = recover_directory(directory, boot, 1013);
    closedir(directory); return result;
}
static int watchdog(int directory, const char *stage, const struct stat *identity, int seconds);

#ifndef SHIZUKA_CAPTURE_TEST
static int daemon_pid(void) {
    DIR *directory = opendir("/proc"); if (!directory) return -1;
    struct dirent *entry; int found = -1;
    static const char expected[] = "/vendor/bin/audioadsprpcd\0audiopd\0adsp";
    while ((entry = readdir(directory))) {
        char *end; long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid < 2 || pid > 2147483647) continue;
        char path[128], command[128]; snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        ssize_t size = read(fd, command, sizeof(command)); close(fd);
        if (size == (ssize_t)sizeof(expected) && !memcmp(command, expected, sizeof(expected))) {
            if (found != -1) { found = -1; break; } found = (int)pid;
        }
    }
    closedir(directory); return found;
}
typedef struct { int number; struct stat identity; } Ring;
static size_t rings(int pid, Ring *out, size_t capacity) {
    char path[128]; snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR *directory = opendir(path); if (!directory) return capacity;
    size_t count = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end; long number = strtol(entry->d_name, &end, 10);
        if (*end || number < 3 || number > 2147483647) continue;
        struct stat info;
        if (snprintf(path, sizeof(path), "/proc/%d/fd/%ld", pid, number) >= (int)sizeof(path)) continue;
        if (stat(path, &info) || info.st_size != RING_SIZE) continue;
        if (count >= capacity) break;
        out[count++] = (Ring){.number = (int)number, .identity = info};
    }
    closedir(directory); return count;
}
static int farf_absent(void) {
    const char *paths[] = {"/data/vendor/audio_dsp", "/vendor/dsp/adsp", "/vendor/dsp",
                          "/vendor/lib/rfsa/adsp", "/vendor/lib/rfsa/dsp", "/vendor/lib64/rfs/dsp"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        DIR *directory = opendir(paths[i]);
        if (!directory) { if (errno == ENOENT) continue; return 0; }
        struct dirent *entry; int clean = 1;
        while ((entry = readdir(directory))) {
            size_t length = strlen(entry->d_name);
            if (length >= 5 && !strcmp(entry->d_name + length - 5, ".farf")) { clean = 0; break; }
        }
        closedir(directory); if (!clean) return 0;
    }
    return 1;
}
#endif
static int watchdog(int directory, const char *stage, const struct stat *identity, int seconds) {
    int parent = (int)getpid(), ready[2];
    int process = (int)syscall(SYS_pidfd_open, parent, 0);
    if (process < 0 || pipe2(ready, O_CLOEXEC)) { if (process >= 0) close(process); return -1; }
    pid_t child = fork();
    if (child < 0) { close(process); close(ready[0]); close(ready[1]); return -1; }
    if (!child) {
        close(ready[0]);
        if (setsid() < 0) _exit(89);
        // A disconnected ADB pipe cannot prevent independent cleanup.
        int null = open("/dev/null", O_RDWR);
        if (null < 0) _exit(90);
        dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null);
        if (write(ready[1], "R", 1) != 1) _exit(91);
        close(ready[1]);
        struct pollfd watched = {.fd = process, .events = POLLIN};
        int status;
        do { status = poll(&watched, 1, (seconds + 5) * 1000); } while (status < 0 && errno == EINTR);
        if (status == 0) {
            syscall(SYS_pidfd_send_signal, process, SIGKILL, NULL, 0);
            // Cleanup only once the sole publisher cannot return from linkat.
            do { status = poll(&watched, 1, 1000); } while (status < 0 && errno == EINTR);
        }
        if (status > 0) {
            unlink_owned(directory, WATCHED, identity);
            unlink_owned(directory, stage, identity);
        }
        close(process); close(directory); _exit(status > 0 ? 0 : 92);
    }
    close(process); close(ready[1]);
    struct pollfd watched = {.fd = ready[0], .events = POLLIN};
    int status = poll(&watched, 1, 1000); char byte = 0;
    if (status > 0) status = (int)read(ready[0], &byte, 1);
    close(ready[0]); return status == 1 && byte == 'R' ? 0 : -1;
}
#ifndef SHIZUKA_CAPTURE_TEST
int main(int argc, char **argv) {
    if (argc != 2 || getuid() != 0) { fputs("root and capture duration required\n", stderr); return 64; }
    if (!strcmp(argv[1], "--recover")) {
        int result = recover();
        printf("{\"type\":\"diagnostic_recovery\",\"result\":%d}\n", result);
        return result;
    }
    char *end; errno = 0; long seconds = strtol(argv[1], &end, 10);
    if (errno || *end || seconds < 5 || seconds > 120) return 64;
    signal(SIGTERM, stop_capture); signal(SIGINT, stop_capture);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int pid = daemon_pid(); if (pid < 2 || !farf_absent()) { fputs("ambiguous daemon or existing FARF configuration\n", stderr); return 65; }
    int process = (int)syscall(SYS_pidfd_open, pid, 0);
    if (process < 0) { perror("daemon pidfd"); return 66; }
    Ring before[128]; size_t count = rings(pid, before, 128);
    if (count >= 128) { close(process); return 67; }
    int directory = open("/data/vendor/audio_dsp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) { close(process); perror("FARF directory"); return 68; }
    char boot[64], stage[128];
    if (boot_id(boot, sizeof(boot))) { close(directory); close(process); return 69; }
    unsigned long long ticks = process_start((int)getpid());
    if (!ticks) { close(directory); close(process); return 69; }
    snprintf(stage, sizeof(stage), ".shizuka-diagnostics-%d-%llu-%s.tmp", (int)getpid(), ticks, boot);
    int file = openat(directory, stage, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat identity = {0};
    if (file < 0) { close(directory); close(process); return 69; }
    int identity_ok = fstat(file, &identity) == 0;
    char label[128];
    ssize_t label_size = fgetxattr(file, "security.selinux", label, sizeof(label));
    static const char expected_label[] = "u:object_r:vendor_audio_dsp_data_file:s0";
    int label_ok = (label_size == sizeof(expected_label) || label_size == sizeof(expected_label)-1) &&
        !memcmp(label, expected_label, sizeof(expected_label)-1) && (label_size == sizeof(expected_label)-1 || label[label_size-1] == 0);
    int okay = identity_ok && label_ok &&
        write(file, farf_config, sizeof(farf_config)-1) == sizeof(farf_config)-1 &&
        fchown(file, 1013, 1013) == 0 && fsync(file) == 0;
    close(file);
    if (!okay || watchdog(directory, stage, &identity, (int)seconds)) {
        fputs("FARF staging or independent cleanup watchdog could not be armed\n", stderr);
        if (identity_ok) unlink_owned(directory, stage, &identity);
        close(directory); close(process); return 70;
    }
    int result = 71, fd = -1; unsigned char *mapping = MAP_FAILED, *copy = NULL;
    double start = monotonic();
    struct pollfd alive = {.fd = process, .events = POLLIN};
    // Recheck immediately before publishing a complete, owned hard link.
    if (!farf_absent() || daemon_pid() != pid || poll(&alive, 1, 0) != 0 || linkat(directory, stage, directory, WATCHED, 0)) {
        fputs("FARF baseline/daemon changed or owned link could not be published\n", stderr); goto cleanup;
    }
    Ring chosen = {0};
    while (!stopped && monotonic() - start < 5) {
        Ring after[128]; size_t size = rings(pid, after, 128); unsigned matches = 0;
        if (size >= 128 || poll(&alive, 1, 0) != 0) goto cleanup;
        for (size_t i = 0; i < size; ++i) {
            int old = 0;
            for (size_t j = 0; j < count; ++j)
                if (after[i].identity.st_dev == before[j].identity.st_dev && after[i].identity.st_ino == before[j].identity.st_ino) old = 1;
            if (!old && (!matches || chosen.identity.st_dev != after[i].identity.st_dev || chosen.identity.st_ino != after[i].identity.st_ino)) {
                chosen = after[i]; matches++;
            }
        }
        if (matches > 1) { fputs("New DMA log ring is ambiguous\n", stderr); goto cleanup; }
        if (matches == 1) break;
        pause_poll();
    }
    if (!chosen.number) { fputs("no unique new stock DSP log ring\n", stderr); goto cleanup; }
    fd = (int)syscall(SYS_pidfd_getfd, process, chosen.number, 0);
    if (fd < 0) { perror("pidfd_getfd"); goto cleanup; }
    struct stat actual;
    if (fd < 0 || fstat(fd, &actual) || actual.st_size != RING_SIZE ||
        actual.st_dev != chosen.identity.st_dev || actual.st_ino != chosen.identity.st_ino) {
        fputs("DMA ring identity changed before mapping\n", stderr); goto cleanup;
    }
    mapping = mmap(NULL, RING_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED || !(copy = malloc(CAPACITY))) { perror("read-only ring mapping/allocation"); goto cleanup; }
    printf("{\"type\":\"capture\",\"daemon_pid\":%d,\"clock\":\"CLOCK_MONOTONIC\",\"ring_overruns\":\"unobservable\"}\n", pid);
    Decoder decoder = {.skip = 1}; uint32_t last = 0; int first = 1; unsigned unstable = 0;
    result = 0;
    while (!stopped && monotonic() - start < seconds) {
        if (poll(&alive, 1, 0) != 0) { result = 72; break; }
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) { perror("DMA read sync"); result = 73; break; }
        uint32_t pos = __atomic_load_n((uint32_t *)(mapping + CAPACITY), __ATOMIC_ACQUIRE);
        memcpy(copy, mapping, CAPACITY);
        uint32_t next = __atomic_load_n((uint32_t *)(mapping + CAPACITY), __ATOMIC_ACQUIRE);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) || pos >= CAPACITY) { fputs("DMA end sync or producer-index validation failed\n", stderr); result = 74; break; }
        if (pos != next) { unstable++; pause_poll(); continue; }
        if (first) { first = 0; last = pos; }
        else while (last != pos) { consume(&decoder, copy[last], monotonic()); last = (last + 1) % CAPACITY; }
        pause_poll();
    }
    printf("{\"type\":\"end\",\"records\":%u,\"discarded_messages\":%u,\"unstable_snapshots\":%u,\"result\":%d}\n",
        decoder.records, decoder.discarded, unstable, result);
cleanup:
    if (mapping != MAP_FAILED) munmap(mapping, RING_SIZE);
    free(copy); if (fd >= 0) close(fd);
    int watched_cleanup = unlink_owned(directory, WATCHED, &identity);
    int stage_cleanup = unlink_owned(directory, stage, &identity);
    if (watched_cleanup || stage_cleanup) {
        fputs("FARF was replaced externally; replacement left untouched\n", stderr); result = 75;
    }
    close(directory); close(process); return result;
}
#endif
