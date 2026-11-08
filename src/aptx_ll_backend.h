// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "aptx_ll.h"
namespace s25::ll {
// Adapter for the normal aptX encoder ABI used by the AOSP/Qualcomm source.
// State ownership stays with the caller. No aptX HD/Adaptive function can be substituted.
struct ClassicEncoder {
    void *state;
    int (*init)(void *, short endian);
    int (*encode_stereo)(void *, void *left, void *right, void *out_words);
};
struct Transport {
    void *context;
    // Must negotiate plan.wire, configure PCM16 and apply plan.packet_bytes/pacing.
    bool (*configure)(void *, const Plan &);
    bool (*send)(void *, const uint8_t *, size_t, uint32_t samples, uint64_t relative_time_us);
    void (*stop)(void *);
};
class ClassicBackend {
  public:
    ClassicBackend(const ClassicEncoder &encoder, const Transport &transport)
        : encoder_(encoder), transport_(transport) {}
    Backend callbacks();

  private:
    ClassicEncoder encoder_;
    Transport transport_;
    uint64_t samples_ = 0;
    uint32_t rate_ = 0;
    static bool configure(void *, const Plan &);
    static bool encode(void *, const uint8_t *, uint8_t *);
    static bool send(void *, const uint8_t *, size_t, uint32_t);
    static void stop(void *);
};
} // namespace s25::ll
