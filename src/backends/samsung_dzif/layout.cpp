// SPDX-License-Identifier: GPL-2.0-only
#include "layout.h"
#include "settings.h"
#include "coexist.h"
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
    if (!a->stream.begin || a->stream.end - a->stream.begin != 11 ||
        a->stream.capacity < a->stream.end)
        return false;
    // Accept only source-compatible, bounded profiles negotiated from the actual peer.
    if (!negotiated_stream_valid(a->stream.begin))
        return false;
    if (a->stream.begin[0] == 0 && a->sample_rate != 48000)
        return false;
    if (a->sample_rate == 44100 && (a->stream.begin[0] != 1 || !(a->stream.begin[1] & 0x80)))
        return false;
    bool balanced = true, stable = true, floored = true;
    for (unsigned i = 0; i < 6; i++) {
        unsigned normal = i % 2 ? 50 : 20;
        if (a->buffering[i] != normal)
            balanced = false;
        if (a->buffering[i] != (i == 2 ? 60 : i == 3 ? 100 : normal))
            stable = false;
        // Margin trial (experiment): minHQ buffering[2] = 100 -> DSP INIT
        // floors TTP at 110 ms instead of 70 ms for the 44.1 HQ stability
        // profile. buffering[2] must stay <= 127: Samsung's offload path
        // sign-extends the byte and larger values silently fall back to the
        // stock default. This only raises the INIT floor (clamped by the
        // DSP's internal HQ maximum, default 200 ms); it does not prove a
        // larger receiver buffer. See docs/TTP-TRIAL.md.
        if (a->buffering[i] != (i == 2 ? 100 : i == 3 ? 100 : normal))
            floored = false;
    }
    return balanced || ((stable || floored) && a->sample_rate == 44100 && a->mode == 4096 &&
                        a->stream.begin[0] == 1 && (a->stream.begin[1] & 0x80));
}
} // namespace s25
