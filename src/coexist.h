// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
namespace s25 {
constexpr int kPrivateAdaptive = 5;
constexpr int kModeHq = 4096;
constexpr int kModeLl = 8192;
constexpr int kModeLossless = 12288;
constexpr uint64_t kRequestDescriptor = 0x5332355200000000ULL;
constexpr uint64_t kDescriptor = 0x5332354100000000ULL;
inline bool flags_valid(uint8_t f) {
    return f < 16 && (f & 3) != 3 && (f & 12) != 12 && (!(f & 8) || (f & 3) == 2);
}
inline bool hd_codec(const uint8_t *c) {
    return c && c[0] == 13 && c[1] == 0 && c[2] == 255 && c[3] == 0xd7 && c[4] == 0 && c[5] == 0 &&
           c[6] == 0 && c[7] == 0x24 && c[8] == 0;
}
inline bool proxy_codec(const uint8_t *c) {
    return hd_codec(c) && c[10] == 0xa5 && c[11] == 0xad && (c[12] < 64 || c[12] == 255) &&
           flags_valid(c[13]);
}
inline void set_proxy(uint8_t *c, uint8_t slot, uint8_t flags) {
    if (hd_codec(c)) {
        c[10] = 0xa5;
        c[11] = 0xad;
        c[12] = slot;
        c[13] = flags;
    }
}
inline void mark_proxy(uint8_t *c) {
    if (hd_codec(c) && !proxy_codec(c))
        set_proxy(c, 255, 0);
}
inline uint64_t descriptor(const uint8_t *c) {
    return proxy_codec(c) ? kDescriptor | (uint64_t(c[12]) << 8) | c[13] : 0;
}
inline bool descriptor_valid(uint64_t d) {
    return (d & 0xffffffff00000000ULL) == kDescriptor && (d & 0xffff0000ULL) == 0 &&
           flags_valid(d & 255) && (((d >> 8) & 255) < 64 || ((d >> 8) & 255) == 255);
}
inline int flags_rate(uint8_t f) { return (f & 3) == 1 ? 96000 : (f & 3) == 2 ? 44100 : 48000; }
inline int flags_mode(uint8_t f) { return (f & 8) ? kModeLossless : (f & 4) ? kModeLl : kModeHq; }
// Public UI alias only. This synthetic ID is never written to AVDTP/HAL.
constexpr int kPublicLossless = 12;
constexpr uint64_t kPublicLosslessId = 0xad01d7ffULL;
// The shipped DSP accepts PCM24/32 containers, including for CD16 Lossless input.
inline int flags_bits(uint8_t) { return 24; }
inline int hal_mode(int mode) { return mode == kModeLl ? kModeLl : kModeHq; }
unsigned proxy_caps(uint8_t slot);
bool proxy_r22(uint8_t slot);
bool proxy_is_selection(uint8_t slot);
bool proxy_choose(uint8_t slot, int rate, int mode, uint8_t *flags);
bool proxy_selection(const uint8_t *proxy, uint8_t *out43);
bool negotiated_stream_valid(const uint8_t *stream);
} // namespace s25
