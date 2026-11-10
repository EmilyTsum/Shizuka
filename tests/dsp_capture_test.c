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
static void automatic_entry_cleanup(int scenario) {
    char path[] = "/tmp/shizuka-dsp-entry-XXXXXX"; assert(mkdtemp(path));
    DIR *directory = opendir(path); assert(directory); int fd = dirfd(directory);
    char boot[64], stage[128]; assert(!boot_id(boot, sizeof(boot)));
    pid_t owner = getpid(); unsigned long long ticks = process_start((int)owner);
    if (scenario != 0 && scenario != 5) {
        owner = fork(); assert(owner >= 0);
        if (!owner) { for (;;) pause(); }
        ticks = process_start((int)owner); assert(ticks);
        assert(!kill(owner, SIGTERM)); int status; assert(waitpid(owner, &status, 0) == owner);
        assert(WIFSIGNALED(status));
    }
    assert(ticks);
    snprintf(stage, sizeof(stage), ".shizuka-diagnostics-%d-%llu-%s.tmp", (int)owner, ticks, boot);
    if (scenario != 5) {
        int file = openat(fd, stage, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0);
        if (scenario == 4) assert(write(file, "different", 9) == 9);
        else assert(write(file, farf_config, sizeof(farf_config)-1) == sizeof(farf_config)-1);
        close(file); assert(!linkat(fd, stage, fd, WATCHED, 0));
    }
    if (scenario == 3 || scenario == 5) {
        if (scenario == 3) assert(!unlinkat(fd, WATCHED, 0));
        int file = openat(fd, WATCHED, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(file >= 0);
        assert(write(file, "replacement", 11) == 11); close(file);
    }
    uid_t expected = scenario == 2 ? getuid() + 1 : getuid();
    int prepared = prepare_capture_directory(directory, boot, expected);
    if (scenario == 1) {
        assert(!prepared); // Dead matching publisher is recovered before new capture.
        assert(faccessat(fd, WATCHED, F_OK, 0) != 0 && faccessat(fd, stage, F_OK, 0) != 0);
        rewinddir(directory); assert(!prepare_capture_directory(directory, boot, expected));
    } else {
        assert(prepared == (scenario == 0 || scenario == 5 ? 65 : 76));
        assert(!faccessat(fd, WATCHED, F_OK, 0));
        if (scenario == 0 || scenario == 2 || scenario == 4) assert(!faccessat(fd, stage, F_OK, 0));
        if (scenario == 3) {
            assert(faccessat(fd, stage, F_OK, 0) != 0);
            int file = openat(fd, WATCHED, O_RDONLY | O_NOFOLLOW); char text[16] = {0}; assert(file >= 0);
            assert(read(file, text, sizeof(text)) == 11 && !strcmp(text, "replacement")); close(file);
            rewinddir(directory); assert(prepare_capture_directory(directory, boot, expected) == 65);
        }
        assert(!unlinkat(fd, WATCHED, 0));
        if (scenario != 3 && scenario != 5) assert(!unlinkat(fd, stage, 0));
    }
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
    Decoder filtered = {.filter = 1};
    static const char spam[] = "CAPI V2 APTX ADAPTIVE ENC:PROCESS() : returning from capi process for packet #12 cycle 5";
    static const char event[] = "GPR MODULE_EVENT source port 0x535a6101 opcode 0x03001000";
    for (unsigned i = 0; i < 50000; ++i) {
        const char *text = i % 2 ? spam : event;
        do { consume(&filtered, (unsigned char)*text, 3); } while (*text++);
    }
    assert(filtered.filtered == 50000 && filtered.records == 0 && !filtered.discarded);
    static const char summary[] = "SPF APTX_ADAPTIVE3_ENC:\nPacket- #9\nHQ State- STATE_ENCLEVEL_4\n";
    for (size_t i = 0; i < sizeof(summary); ++i) consume(&filtered, summary[i], 3.1);
    assert(filtered.records == 1 && filtered.filtered == 50000);
    assert(relevant("CAPI V2 APTX_ADAPTIVE_ENC: set-param Wrapper - me->linkState.sampleRate is 96000"));
    assert(relevant("IMCL_PARAM_ID_BT_BIT_RATE_LEVEL_ENCODER_FEEDBACK br_level =  5"));
    assert(relevant("CAPI APTX ADAPTIVE ENC: IMCL_PARAM_ID_BT_BIT_RATE_LEVEL_ENCODER_FEEDBACK br_level =  5"));
    assert(relevant("SPF APTX_ADAPTIVE3_ENC: quality level received is 4"));
    assert(relevant("Segment [0] streamed SigVerify for aptx_adaptive_enc_module.so.1"));
    assert(!relevant("Other encoder System mode is 3"));
    static const char *const output_records[] = {
        "SPF APTX_ADAPTIVE3_ENC: Packet type is 1. Packet size is 664. Payload size is 656",
        "CAPI V2 APTX ADAPTIVE ENC:PROCESS()- dispatching Left Enc pkt #12 at cycle 52, output[port]->buf_ptr[0].data_ptr is 0x1234abcd, actual_data_len = 336",
        "CAPI V2 APTX ADAPTIVE ENC:PROCESS()- dispatching Right Enc pkt #12 at cycle 53, output[port]->buf_ptr[0].data_ptr is 0x1234abcd, actual_data_len = 336",
        "SPF APTX_ADAPTIVE3_ENC: Packet type 1, bad size 665 (should be 656)"
    };
    for (unsigned record = 0; record < sizeof(output_records)/sizeof(output_records[0]); ++record) {
        const char *text = output_records[record]; assert(relevant(text));
        do { consume(&filtered, (unsigned char)*text, 3.2); } while (*text++);
    }
    assert(filtered.records == 5 && filtered.filtered == 50000 && !filtered.discarded);
    assert(!relevant("unrelated encoder: Packet type is 1. Packet size is 664. Payload size is 656"));
    assert(!relevant("APTX other output actual_data_len = 664"));
    assert(process_start((int)getpid()) > 0);
    deadline_cleanup(0); deadline_cleanup(1);
    stale_boot_cleanup();
    for (int scenario = 0; scenario < 6; ++scenario) automatic_entry_cleanup(scenario);
    puts("NATIVE_CAPTURE_PASSED");
    return 0;
}
