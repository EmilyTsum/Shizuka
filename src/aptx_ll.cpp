// SPDX-License-Identifier: GPL-2.0-only
#include "aptx_ll.h"
namespace s25::ll {
static void copy(uint8_t *d, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
}
static bool same(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 6; i++)
        if (a[i] != b[i])
            return false;
    return true;
}
static uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
Error parse(const uint8_t *c, size_t n, Capabilities *out) {
    if (!c || !out || (n != 11 && n != 20) || c[0] != n - 1 || c[1] || c[2] != 255 || c[4] ||
        c[5] || c[6] || c[7] != 2 || c[8] || c[10] & 0xfc)
        return Error::Malformed;
    if (c[3] != 0x0a && c[3] != 0xd7)
        return Error::Unsupported;
    bool extended = (c[10] & 2) != 0;
    if (n != (extended ? 20u : 11u))
        return Error::Malformed;
    if (!(c[9] & 2) || !(c[9] & 0x30))
        return Error::Unsupported;
    if ((c[9] & 15) & ~3)
        return Error::Malformed;
    Capabilities value{};
    value.vendor = c[3];
    value.rates = c[9] & 0x30;
    value.extended = extended;
    value.backchannel = (c[10] & 1) != 0;
    value.target = 180;
    value.initial = 360;
    value.working = 180;
    value.sra_max = 50;
    value.sra_average = 1;
    if (extended) {
        if (c[11])
            return Error::Malformed;
        value.target = get16(c + 12);
        value.initial = get16(c + 14);
        value.working = get16(c + 18);
        value.sra_max = c[16];
        value.sra_average = c[17];
        if (!value.target)
            value.target = 180;
        if (!value.initial)
            value.initial = 360;
        if (!value.working)
            value.working = 180;
        if (!value.sra_max)
            value.sra_max = 50;
        if (!value.sra_average)
            value.sra_average = 1;
        if (value.initial < value.target || value.working > value.initial)
            return Error::Malformed;
    }
    *out = value;
    return Error::Ok;
}
Error negotiate(const uint8_t *c, size_t n, const Request &r, Plan *out) {
    if (!out)
        return Error::Malformed;
    Capabilities caps{};
    Error error = parse(c, n, &caps);
    if (error != Error::Ok)
        return error;
    // Music-only scope: a bidirectional sink is allowed, but no return link is requested.
    if (r.backchannel || r.bits != 16 || (r.rate != 44100 && r.rate != 48000))
        return Error::Unsupported;
    uint8_t rate = r.rate == 48000 ? 0x10 : 0x20;
    if (!(caps.rates & rate))
        return Error::Unsupported;
    if (r.peer_mtu < 4 || r.peer_mtu > 65535)
        return Error::Malformed;
    Plan value{};
    value.wire_size = n;
    copy(value.wire, c, n);
    value.wire[9] = rate | 2;
    value.wire[10] = caps.extended ? 2 : 0;
    if (caps.extended) {
        put16(value.wire + 12, caps.target);
        put16(value.wire + 14, caps.initial);
        put16(value.wire + 18, caps.working);
        value.wire[16] = caps.sra_max;
        value.wire[17] = caps.sra_average;
    }
    const uint8_t aptx[10] = {9, 0, 255, 0x4f, 0, 0, 0, 1, 0, uint8_t(rate | 2)};
    copy(value.aptx, aptx, 10);
    value.rate = r.rate;
    value.bits = 16;
    value.channels = 2;
    size_t frames = (uint64_t(r.rate) * 7500 / 1000000) / 4;
    if (frames > r.peer_mtu / 4)
        frames = r.peer_mtu / 4;
    value.frames_per_packet = uint16_t(frames);
    value.packet_bytes = uint16_t(frames * 4);
    if (!frames || value.packet_bytes > 360)
        return Error::Malformed;
    *out = value;
    return Error::Ok;
}
Error Session::discover(const uint8_t *address, const uint8_t *cie, size_t size) {
    if (!address)
        return Error::Malformed;
    Capabilities caps{};
    Error error = parse(cie, size, &caps);
    if (error != Error::Ok)
        return error;
    // No silent replacement of a live peer's transport/encoder state.
    if (active_)
        return Error::BackendFailure;
    copy(address_, address, 6);
    copy(cie_, cie, size);
    cie_size_ = size;
    return Error::Ok;
}
Error Session::bind(const Backend &b) {
    if (active_ || !b.configure || !b.encode_frame || !b.send_packet || !b.stop)
        return Error::BackendUnavailable;
    backend_ = b;
    return Error::Ok;
}
bool Session::available(const uint8_t *address) const {
    return address && cie_size_ && same(address_, address) && backend_.configure &&
           backend_.encode_frame && backend_.send_packet && backend_.stop;
}
Error Session::begin(const uint8_t *address, const Request &r) {
    if (!address || !cie_size_)
        return Error::NoPeer;
    if (!same(address_, address))
        return Error::WrongPeer;
    if (!available(address))
        return Error::BackendUnavailable;
    if (active_)
        return Error::BackendFailure;
    Plan next{};
    Error error = negotiate(cie_, cie_size_, r, &next);
    if (error != Error::Ok)
        return error;
    if (!backend_.configure(backend_.context, next)) {
        backend_.stop(backend_.context);
        return Error::BackendFailure;
    }
    plan_ = next;
    packet_size_ = 0;
    active_ = true;
    return Error::Ok;
}
Error Session::flush() {
    if (!active_)
        return Error::BackendUnavailable;
    if (!packet_size_)
        return Error::Ok;
    if (!backend_.send_packet(backend_.context, packet_, packet_size_, uint32_t(packet_size_))) {
        stop();
        return Error::BackendFailure;
    }
    packet_size_ = 0;
    return Error::Ok;
}
Error Session::encode(const uint8_t *pcm, size_t bytes) {
    if (!active_)
        return Error::BackendUnavailable;
    if ((!pcm && bytes) || bytes % 16)
        return Error::Malformed;
    for (size_t i = 0; i < bytes; i += 16) {
        if (!backend_.encode_frame(backend_.context, pcm + i, packet_ + packet_size_)) {
            stop();
            return Error::BackendFailure;
        }
        packet_size_ += 4;
        if (packet_size_ == plan_.packet_bytes) {
            Error error = flush();
            if (error != Error::Ok)
                return error;
        }
    }
    return Error::Ok;
}
void Session::stop() {
    if (active_ && backend_.stop)
        backend_.stop(backend_.context);
    active_ = false;
    packet_size_ = 0;
}
void Session::reset() {
    stop();
    backend_ = {};
    cie_size_ = 0;
    plan_ = {};
    for (auto &b : address_)
        b = 0;
}
Session &android_session() {
    static Session session;
    return session;
}
} // namespace s25::ll
