// SPDX-License-Identifier: GPL-2.0-only
#include "latency_context.h"
#include "layout.h"
#include "coexist.h"

extern "C" {
__attribute__((visibility("hidden"), aligned(16))) S25LatencyContext s25_latency_context;
}
namespace {
uint8_t writer_lock;
struct Guard {
    Guard() { while (__atomic_test_and_set(&writer_lock, __ATOMIC_ACQUIRE)) {} }
    ~Guard() { __atomic_clear(&writer_lock, __ATOMIC_RELEASE); }
};
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
uint32_t byte_hash(uint32_t h, uint8_t v) { return (h ^ v) * UINT32_C(16777619); }
uint32_t word_hash(uint32_t h, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) h = byte_hash(h, uint8_t(v >> (i * 8)));
    return h;
}
size_t stream_size(const s25::AdaptiveConfig &a) {
    uintptr_t b = reinterpret_cast<uintptr_t>(a.stream.begin);
    uintptr_t e = reinterpret_cast<uintptr_t>(a.stream.end);
    uintptr_t c = reinterpret_cast<uintptr_t>(a.stream.capacity);
    return b && e >= b && c >= e ? e - b : 0;
}
struct ValidatedAllocation {
    const uint8_t *cfg;
    const uint8_t *begin;
    const uint8_t *capacity;
    uint32_t fingerprint;
    uint32_t sequence;
    uint32_t tag[4];
} prior;
void store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
uint32_t publish(bool valid, uint32_t rate, uint32_t mode, uint32_t flags, uint32_t fingerprint) {
    auto &c = s25_latency_context;
    uint32_t next = (load(&c.sequence) + 2) & ~UINT32_C(1);
    if (!next) next = 2;
    __atomic_store_n(&c.sequence, next - 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE); // publish odd before any payload stores
    store(&c.magic, S25_LATENCY_CONTEXT_MAGIC);
    store(&c.abi, S25_LATENCY_CONTEXT_ABI);
    store(&c.size, sizeof c);
    store(&c.valid, valid ? 1 : 0);
    store(&c.sample_rate, rate);
    store(&c.mode, mode);
    store(&c.private_flags, flags);
    store(&c.config_fingerprint, fingerprint);
    for (auto &r : c.reserved) store(&r, 0);
    __atomic_store_n(&c.sequence, next, __ATOMIC_RELEASE);
    return next;
}
void invalidate_locked() {
    prior = {};
    publish(false, 0, 0, 0, 0);
}
uint32_t tag_checksum(const uint8_t *tag, uint32_t fingerprint) {
    uint32_t h = UINT32_C(2166136261);
    for (unsigned i = 0; i != 12; ++i) h = byte_hash(h, tag[i]);
    return word_hash(h, fingerprint);
}
bool tag_fields_valid(const s25::AdaptiveConfig &a, const uint8_t *tag, uint32_t fp) {
    const uint32_t flags = le32(tag + 4);
    return le32(tag) == S25_LATENCY_TAG_MAGIC && flags < 16 &&
           s25::flags_valid(uint8_t(flags)) &&
           le32(tag + 8) == uint32_t(a.sample_rate) &&
           s25::flags_rate(uint8_t(flags)) == a.sample_rate &&
           s25::hal_mode(s25::flags_mode(uint8_t(flags))) == a.mode &&
           s25::flags_bits(uint8_t(flags)) == a.bits &&
           le32(tag + 12) == tag_checksum(tag, fp);
}
} // namespace

namespace s25 {
uint32_t latency_config_fingerprint(const uint8_t *cfg) {
    const auto &a = *reinterpret_cast<const AdaptiveConfig *>(cfg + 0x18);
    uint32_t h = word_hash(UINT32_C(2166136261), UINT32_C(0x31464341)); // ACF1
    h = word_hash(h, uint32_t(a.sample_rate));
    h = word_hash(h, uint32_t(a.channel_mode));
    h = word_hash(h, a.bits);
    h = word_hash(h, uint32_t(a.mode));
    for (uint8_t v : a.buffering) h = byte_hash(h, v);
    for (int8_t v : a.ttp) h = byte_hash(h, uint8_t(v));
    h = word_hash(h, uint32_t(a.input_mode));
    h = word_hash(h, uint32_t(a.fade_ms));
    for (unsigned i = 0; i != S25_LATENCY_STREAM_BYTES; ++i)
        h = byte_hash(h, a.stream.begin[i]);
    return h;
}
bool latency_tag_write(uint8_t *cfg, uint8_t flags) {
    if (!cfg || le32(cfg) != 9 || le32(cfg + 0x58) != 4) return false;
    auto &a = *reinterpret_cast<AdaptiveConfig *>(cfg + 0x18);
    if (stream_size(a) != S25_LATENCY_TAGGED_BYTES || !flags_valid(flags) ||
        flags_rate(flags) != a.sample_rate || hal_mode(flags_mode(flags)) != a.mode ||
        flags_bits(flags) != a.bits) return false;
    uint8_t *tag = a.stream.begin + S25_LATENCY_STREAM_BYTES;
    put32(tag, S25_LATENCY_TAG_MAGIC);
    put32(tag + 4, flags);
    put32(tag + 8, uint32_t(a.sample_rate));
    put32(tag + 12, tag_checksum(tag, latency_config_fingerprint(cfg)));
    return true;
}
bool latency_tag_valid(const uint8_t *cfg) {
    if (!cfg || le32(cfg) != 9 || le32(cfg + 0x58) != 4) return false;
    const auto &a = *reinterpret_cast<const AdaptiveConfig *>(cfg + 0x18);
    return stream_size(a) == S25_LATENCY_TAGGED_BYTES &&
           tag_fields_valid(a, a.stream.begin + S25_LATENCY_STREAM_BYTES,
                            latency_config_fingerprint(cfg));
}
void latency_context_invalidate() {
    Guard guard;
    invalidate_locked();
}
bool latency_validate_and_capture(uint8_t session, uint8_t *cfg) {
    // Validate before trimming: a corrupt tag must never become accepted as legacy.
    bool accepted = (session == 2 || session == 11) && adaptive_valid(cfg);
    bool adaptive = cfg && le32(cfg) == 9 && le32(cfg + 0x58) == 4;
    auto *a = adaptive ? reinterpret_cast<AdaptiveConfig *>(cfg + 0x18) : nullptr;
    size_t n = a ? stream_size(*a) : 0;
    Guard guard;
    if (accepted && n == S25_LATENCY_TAGGED_BYTES) {
        uint8_t *tag = a->stream.begin + S25_LATENCY_STREAM_BYTES;
        uint32_t fp = latency_config_fingerprint(cfg);
        uint32_t flags = le32(tag + 4);
        prior = {cfg, a->stream.begin, a->stream.capacity, fp, 0, {0, 0, 0, 0}};
        for (unsigned i = 0; i != 4; ++i) prior.tag[i] = le32(tag + 4 * i);
        // No copy after this function can serialize the APSS-only suffix.
        a->stream.end = a->stream.begin + S25_LATENCY_STREAM_BYTES;
        prior.sequence = publish(true, uint32_t(a->sample_rate),
                                 uint32_t(flags_mode(uint8_t(flags))), flags, fp);
    } else if (accepted && n == S25_LATENCY_STREAM_BYTES && prior.cfg == cfg &&
               prior.begin == a->stream.begin && prior.capacity == a->stream.capacity &&
               prior.sequence == load(&s25_latency_context.sequence) &&
               load(&s25_latency_context.valid) == 1 &&
               prior.fingerprint == latency_config_fingerprint(cfg)) {
        // Only a previously observed full allocation authorizes reading its tail.
        bool same = true;
        for (unsigned i = 0; i != 4; ++i)
            same &= prior.tag[i] == le32(a->stream.begin + S25_LATENCY_STREAM_BYTES + 4 * i);
        if (!same) invalidate_locked();
    } else {
        if (n == S25_LATENCY_TAGGED_BYTES)
            a->stream.end = a->stream.begin + S25_LATENCY_STREAM_BYTES;
        invalidate_locked();
    }
    return accepted;
}
} // namespace s25

extern "C" int s25_latency_context_snapshot(S25LatencyContext *out) {
    if (!out) return 0;
    const auto &c = s25_latency_context;
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        uint32_t seq = __atomic_load_n(&c.sequence, __ATOMIC_ACQUIRE);
        if (!seq || (seq & 1)) continue;
        out->magic = load(&c.magic);
        out->abi = load(&c.abi);
        out->size = load(&c.size);
        out->valid = load(&c.valid);
        out->sample_rate = load(&c.sample_rate);
        out->mode = load(&c.mode);
        out->private_flags = load(&c.private_flags);
        out->config_fingerprint = load(&c.config_fingerprint);
        for (unsigned i = 0; i != 7; ++i) out->reserved[i] = load(&c.reserved[i]);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq == __atomic_load_n(&c.sequence, __ATOMIC_ACQUIRE)) {
            out->sequence = seq;
            return 1;
        }
    }
    return 0;
}
