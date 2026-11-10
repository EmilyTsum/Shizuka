// SPDX-License-Identifier: GPL-2.0-only
#include "layout.h"
#include "settings.h"
#include "coexist.h"
#include "latency_context.h"
namespace s25 {
static uint32_t u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool adaptive_valid(const uint8_t *cfg) {
    if (!cfg || u32(cfg) != 9 || u32(cfg + 0x58) != 4)
        return false;
    const AdaptiveConfig *a = reinterpret_cast<const AdaptiveConfig *>(cfg + 0x18);
    if ((a->sample_rate != 48000 && a->sample_rate != 96000 &&
         !(kR2 == 3 && a->sample_rate == 44100)) ||
        a->channel_mode != 0 || a->bits != 24 || (a->mode != 4096 && a->mode != 8192) ||
        a->input_mode != 0 || a->fade_ms != 255)
        return false;
    uintptr_t begin = reinterpret_cast<uintptr_t>(a->stream.begin);
    uintptr_t end = reinterpret_cast<uintptr_t>(a->stream.end);
    uintptr_t capacity = reinterpret_cast<uintptr_t>(a->stream.capacity);
    if (!begin || end < begin || capacity < end ||
        (end - begin != S25_LATENCY_STREAM_BYTES && end - begin != S25_LATENCY_TAGGED_BYTES))
        return false;
    // Accept only source-compatible, bounded profiles negotiated from the actual peer.
    if (!negotiated_stream_valid(a->stream.begin))
        return false;
    if (a->stream.begin[0] == 0 && a->sample_rate != 48000)
        return false;
    if (a->sample_rate == 44100 && (a->stream.begin[0] != 1 || !(a->stream.begin[1] & 0x80)))
        return false;
    bool balanced = true, stable = true;
    for (unsigned i = 0; i < 6; i++) {
        unsigned normal = i % 2 ? 50 : 20;
        if (a->buffering[i] != normal)
            balanced = false;
        if (a->buffering[i] != (i == 2 ? 60 : i == 3 ? 100 : normal))
            stable = false;
    }
    bool bounds_valid = balanced || (stable && a->sample_rate == 44100 && a->mode == 4096 &&
                                    a->stream.begin[0] == 1 && (a->stream.begin[1] & 0x80));
    return bounds_valid && (end - begin == S25_LATENCY_STREAM_BYTES || latency_tag_valid(cfg));
}
} // namespace s25
