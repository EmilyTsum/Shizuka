// SPDX-License-Identifier: GPL-2.0-only
#include "../src/latency_context.h"
#include "../src/layout.h"
#include "../src/coexist.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <initializer_list>

extern "C" bool s25_validate(const uint8_t *, const uint8_t *);
extern "C" void s25_report_config(void *, const uint8_t *);
extern "C" void s25_anchor() {}
static unsigned validate_calls, report_calls;
extern "C" bool tramp_validate(const uint8_t *, const uint8_t *) {
    ++validate_calls;
    return true;
}
extern "C" void tramp_report_config(void *, const uint8_t *cfg) {
    ++report_calls;
    if (*(const uint32_t *)(cfg + 0x68) == 1 && *(const uint32_t *)cfg == 9) {
        const auto *a = reinterpret_cast<const s25::AdaptiveConfig *>(cfg + 0x18);
        // The actual session copy and later callback both see the same logical length.
        assert(a->stream.end - a->stream.begin == 11);
        uint8_t serialized[11];
        memcpy(serialized, a->stream.begin, a->stream.end - a->stream.begin);
        assert(serialized[0] == 1 && serialized[10] == 0xaa);
    }
}
struct Config {
    alignas(16) uint8_t cfg[112]{};
    uint8_t bytes[27]{};
    s25::AdaptiveConfig &a() { return *reinterpret_cast<s25::AdaptiveConfig *>(cfg + 0x18); }
    void reset(uint8_t flags = 2, bool tagged = true, bool stable = false) {
        memset(cfg, 0, sizeof cfg);
        memset(bytes, 0, sizeof bytes);
        *(uint32_t *)cfg = 9;
        *(uint32_t *)(cfg + 0x58) = 4;
        *(uint32_t *)(cfg + 0x68) = 1;
        a().sample_rate = s25::flags_rate(flags);
        a().bits = 24;
        a().mode = s25::hal_mode(s25::flags_mode(flags));
        a().fade_ms = 255;
        for (unsigned i = 0; i != 6; ++i) {
            a().buffering[i] = i % 2 ? 50 : 20;
            a().ttp[i] = int8_t(i * 3 - 8);
        }
        if (stable) { a().buffering[2] = 60; a().buffering[3] = 100; }
        const uint8_t negotiated[11] = {1, 0x97, 0, 0, 15, 2, 3, 3, 3, 0, 0xaa};
        memcpy(bytes, negotiated, sizeof negotiated);
        a().stream = {bytes, bytes + (tagged ? 27 : 11), bytes + 27};
        if (tagged) assert(s25::latency_tag_write(cfg, flags));
    }
};
static S25LatencyContext snapshot() {
    S25LatencyContext c{};
    assert(s25_latency_context_snapshot(&c));
    assert(c.magic == S25_LATENCY_CONTEXT_MAGIC && c.abi == 1 && c.size == 64);
    assert(c.sequence && !(c.sequence & 1));
    for (auto r : c.reserved) assert(!r);
    return c;
}
int main() {
    const uint8_t session = 2;
    Config f;
    f.reset(2, false);
    assert(s25::adaptive_valid(f.cfg));
    assert(s25_validate(&session, f.cfg));
    assert(!snapshot().valid); // legacy mode is never invented

    f.reset(2, true, true); // minHQ60 is also legal HQ; it is not a Lossless marker
    uint8_t original[11];
    memcpy(original, f.bytes, 11);
    assert(s25::adaptive_valid(f.cfg));
    uint32_t fp = s25::latency_config_fingerprint(f.cfg);
    assert(s25_validate(&session, f.cfg));
    auto hq = snapshot();
    assert(hq.valid && hq.mode == 4096 && hq.private_flags == 2 && hq.sample_rate == 44100);
    assert(hq.config_fingerprint == fp && f.a().stream.end == f.bytes + 11);
    assert(f.a().stream.capacity == f.bytes + 27 && !memcmp(f.bytes, original, 11));
    assert(s25_validate(&session, f.cfg));
    assert(snapshot().sequence == hq.sequence); // repeated validation of the same trimmed object

    Config legacy_clone;
    legacy_clone.reset(2, false, true);
    memcpy(legacy_clone.bytes + 11, f.bytes + 11, 16); // forged spare-capacity tail is not authority
    assert(s25_validate(&session, legacy_clone.cfg));
    assert(!snapshot().valid);
    assert(s25_validate(&session, f.cfg));
    assert(!snapshot().valid); // old cache cannot revive after another config/invalidation

    f.reset(10, true, true);
    assert(s25_validate(&session, f.cfg));
    auto lossless = snapshot();
    assert(lossless.valid && lossless.mode == 12288 && lossless.private_flags == 10);
    assert(f.a().mode == 4096); // signed DSP still sees its accepted HQ enum
    assert(lossless.sequence != hq.sequence);
    f.bytes[11] ^= 1; // tail changed after trim: retain baseline config, revoke authority
    assert(s25_validate(&session, f.cfg));
    assert(!snapshot().valid);

    for (uint8_t flags : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(4), uint8_t(5),
                          uint8_t(6), uint8_t(10)}) {
        f.reset(flags);
        assert(s25_validate(&session, f.cfg));
        auto c = snapshot();
        assert(c.valid && c.mode == uint32_t(s25::flags_mode(flags)) &&
               c.sample_rate == uint32_t(s25::flags_rate(flags)) && c.private_flags == flags);
    }
    for (unsigned byte = 0; byte != 16; ++byte) {
        f.reset();
        f.bytes[11 + byte] ^= 0x40;
        assert(!s25::adaptive_valid(f.cfg));
        assert(!s25_validate(&session, f.cfg));
        assert(!snapshot().valid && f.a().stream.end == f.bytes + 11);
    }
    f.reset();
    f.a().sample_rate = 48000; // tag rate and actual config must agree
    assert(!s25_validate(&session, f.cfg) && !snapshot().valid);
    f.reset();
    f.a().mode = 8192;
    assert(!s25_validate(&session, f.cfg) && !snapshot().valid);
    f.reset();
    f.a().buffering[0]++;
    assert(!s25_validate(&session, f.cfg) && !snapshot().valid);
    f.reset();
    f.a().stream.end = f.bytes + 26;
    assert(!s25_validate(&session, f.cfg) && !snapshot().valid);
    f.reset();
    f.a().stream.capacity = f.bytes + 26;
    assert(!s25_validate(&session, f.cfg) && !snapshot().valid);
    f.reset();
    uint8_t wrong_session = 1;
    assert(!s25_validate(&wrong_session, f.cfg) && !snapshot().valid);

    // Padding, pointers, and logical vector length do not alter the normalized hash.
    f.reset();
    fp = s25::latency_config_fingerprint(f.cfg);
    memset(f.a().padding, 0x77, sizeof f.a().padding);
    memset(f.a().align, 0x99, sizeof f.a().align);
    assert(s25::latency_config_fingerprint(f.cfg) == fp);
    assert(s25_validate(&session, f.cfg));
    assert(s25::latency_config_fingerprint(f.cfg) == fp);
    f.a().ttp[0]++;
    assert(s25::latency_config_fingerprint(f.cfg) != fp);
    assert(s25_validate(&session, f.cfg) && !snapshot().valid);

    // UPDATE never calls stock validator; the second hook strips before its copy/callback.
    uint8_t session_object[0x30]{};
    session_object[0x28] = 2;
    f.reset(10, true, true);
    s25_report_config(session_object, f.cfg);
    assert(report_calls == 1 && snapshot().valid && snapshot().mode == 12288);
    f.reset();
    f.bytes[26] ^= 1;
    s25_report_config(session_object, f.cfg);
    assert(report_calls == 2 && !snapshot().valid); // malformed tag is stripped, never forwarded
    f.reset(2, false);
    s25_report_config(session_object, f.cfg);
    assert(report_calls == 3 && !snapshot().valid);
    *(uint32_t *)f.cfg = 4;
    assert(s25_validate(&session, f.cfg));
    assert(validate_calls == 1 && !snapshot().valid);
    s25_report_config(session_object, f.cfg);
    assert(report_calls == 4 && !snapshot().valid);

    // Independent readers must never combine the rate/flags/mode of two publications.
    f.reset();
    assert(s25_validate(&session, f.cfg));
    std::atomic<bool> done{false};
    std::atomic<bool> reading{false};
    std::thread writer([&] {
        while (!reading.load()) std::this_thread::yield();
        Config local;
        for (unsigned i = 0; i != 10000; ++i) {
            local.reset(i & 1 ? 1 : 10, true, !(i & 1));
            assert(s25_validate(&session, local.cfg));
        }
        done.store(true);
    });
    unsigned reads = 1;
    // This handshake also makes the concurrency test deterministic on a busy host.
    assert(snapshot().valid);
    reading.store(true);
    while (!done.load()) {
        S25LatencyContext c{};
        if (s25_latency_context_snapshot(&c) && c.valid) {
            assert(c.sample_rate == uint32_t(s25::flags_rate(uint8_t(c.private_flags))));
            assert(c.mode == uint32_t(s25::flags_mode(uint8_t(c.private_flags))));
            ++reads;
        }
    }
    writer.join();
    assert(reads);
    assert(!s25_latency_context_snapshot(nullptr));
    puts("latency context: tagged HQ/LL/Lossless, legacy invalidation, copy/trim, corrupt tags, "
         "start/update wrappers, and concurrent seqlock snapshots passed");
}
