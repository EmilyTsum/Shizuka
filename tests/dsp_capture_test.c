// SPDX-License-Identifier: GPL-2.0-only
#define SHIZUKA_CAPTURE_TEST
#include "../src/diagnostics/dsp_capture.c"
#include <assert.h>

static void deadline_cleanup(int replacement) {
    char path[] = "/tmp/shizuka-dsp-test-XXXXXX"; assert(mkdtemp(path));
    int directory = open(path, O_RDONLY | O_DIRECTORY); assert(directory >= 0);
    int file = openat(directory, "stage", O_CREAT | O_EXCL | O_RDWR, 0600); assert(file >= 0);
    struct stat identity; assert(!fstat(file, &identity)); close(file);
    assert(!linkat(directory, "stage", directory, WATCHED, 0));
    if (replacement) {
        assert(!unlinkat(directory, WATCHED, 0));
        file = openat(directory, WATCHED, O_CREAT | O_EXCL | O_RDWR, 0600); assert(file >= 0); close(file);
    }
    pid_t worker = fork(); assert(worker >= 0);
    if (!worker) {
        // The production entry point accepts 5..120 seconds. A short test-only
        // deadline exercises the exact detached watchdog against our own worker.
        if (watchdog(directory, "stage", &identity, -4)) _exit(1);
        for (;;) pause();
    }
    int status; assert(waitpid(worker, &status, 0) == worker);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    for (int i = 0; i < 100 && faccessat(directory, "stage", F_OK, 0) == 0; ++i) pause_poll();
    assert(faccessat(directory, "stage", F_OK, 0) != 0);
    assert((faccessat(directory, WATCHED, F_OK, 0) == 0) == replacement);
    if (replacement) assert(!unlinkat(directory, WATCHED, 0));
    close(directory); assert(!rmdir(path));
}
static void stale_boot_cleanup(void) {
    char path[] = "/tmp/shizuka-dsp-recover-XXXXXX"; assert(mkdtemp(path));
    DIR *directory = opendir(path); assert(directory); int fd = dirfd(directory);
    char boot[64], stage[128]; assert(!boot_id(boot, sizeof(boot)));
    snprintf(stage, sizeof(stage), ".shizuka-diagnostics-%d-%llu-%s.tmp", (int)getpid(), process_start((int)getpid()), boot);
    int file = openat(fd, stage, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0);
    assert(write(file, farf_config, sizeof(farf_config)-1) == sizeof(farf_config)-1); close(file);
    assert(!linkat(fd, stage, fd, WATCHED, 0));
    assert(!recover_directory(directory, boot, getuid()));
    assert(!faccessat(fd, WATCHED, F_OK, 0)); // Current owner is alive; recovery must leave it.
    rewinddir(directory);
    char previous_boot[64]; strcpy(previous_boot, boot); previous_boot[0] = previous_boot[0] == 'a' ? 'b' : 'a';
    assert(!recover_directory(directory, previous_boot, getuid()));
    assert(faccessat(fd, WATCHED, F_OK, 0) != 0 && faccessat(fd, stage, F_OK, 0) != 0);
    closedir(directory); assert(!rmdir(path));
}
int main(void) {
    Decoder decoder = {.skip = 1};
    const unsigned char message[] = "partial\0A \"quoted\"\\line\nnext\0";
    for (size_t i = 0; i < sizeof(message)-1; ++i) consume(&decoder, message[i], 1.25);
    assert(decoder.records == 1 && decoder.discarded == 0);
    for (size_t i = 0; i < MAX_MESSAGE + 3; ++i) consume(&decoder, 'X', 2);
    consume(&decoder, 0, 2); assert(decoder.records == 1 && decoder.discarded == 1);
    consume(&decoder, 255, 2); consume(&decoder, 0, 2); assert(decoder.discarded == 2);
    assert(process_start((int)getpid()) > 0);
    deadline_cleanup(0); deadline_cleanup(1);
    stale_boot_cleanup();
    puts("NATIVE_CAPTURE_PASSED");
    return 0;
}
