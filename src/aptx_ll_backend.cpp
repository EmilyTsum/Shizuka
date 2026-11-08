// SPDX-License-Identifier: GPL-2.0-only
#include "aptx_ll_backend.h"
namespace s25::ll {
Backend ClassicBackend::callbacks() {
    if (!encoder_.state || !encoder_.init || !encoder_.encode_stereo || !transport_.configure ||
        !transport_.send || !transport_.stop)
        return {};
    return {this, configure, encode, send, stop};
}
bool ClassicBackend::configure(void *context, const Plan &p) {
    auto &self = *(ClassicBackend *)context;
    if (p.bits != 16 || p.channels != 2 || p.aptx[3] != 0x4f || p.aptx[7] != 1 || !p.packet_bytes ||
        p.packet_bytes > 360)
        return false;
    if (!self.transport_.configure(self.transport_.context, p))
        return false;
    // Qualcomm's AOSP caller does not define/check a success return value for this ABI.
    // Do not invent a meaning for it here; the native adapter remains unverified until device
    // tests.
    self.encoder_.init(self.encoder_.state, 0);
    self.rate_ = p.rate;
    self.samples_ = 0;
    return true;
}
bool ClassicBackend::encode(void *context, const uint8_t *in, uint8_t *out) {
    auto &self = *(ClassicBackend *)context;
    uint32_t left[4], right[4];
    uint16_t words[2]{};
    for (unsigned i = 0; i < 4; i++) {
        left[i] = uint32_t(in[4 * i]) | (uint32_t(in[4 * i + 1]) << 8);
        right[i] = uint32_t(in[4 * i + 2]) | (uint32_t(in[4 * i + 3]) << 8);
    }
    self.encoder_.encode_stereo(self.encoder_.state, left, right, words);
    out[0] = words[0] >> 8;
    out[1] = words[0];
    out[2] = words[1] >> 8;
    out[3] = words[1];
    return true;
}
bool ClassicBackend::send(void *context, const uint8_t *data, size_t size, uint32_t samples) {
    auto &self = *(ClassicBackend *)context;
    if (!self.rate_)
        return false;
    // Derive time from exact PCM sample count, including 44.1kHz's fractional packet duration.
    // The platform send callback schedules against its monotonic stream origin.
    uint64_t time = self.samples_ * 1000000 / self.rate_;
    if (!self.transport_.send(self.transport_.context, data, size, samples, time))
        return false;
    self.samples_ += samples;
    return true;
}
void ClassicBackend::stop(void *context) {
    auto &self = *(ClassicBackend *)context;
    self.transport_.stop(self.transport_.context);
    self.rate_ = 0;
    self.samples_ = 0;
}
} // namespace s25::ll
