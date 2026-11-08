// SPDX-License-Identifier: GPL-2.0-only
#include "../src/aptx_ll.h"
#include "../src/aptx_ll_backend.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
using namespace s25::ll;
struct Fake {
    unsigned configured = 0, stopped = 0, encoded = 0;
    bool fail_send = false;
    std::vector<unsigned> sizes;
    Plan plan{};
};
static bool configure(void *p, const Plan &plan) {
    auto &f = *(Fake *)p;
    f.configured++;
    f.plan = plan;
    return true;
}
static bool encode(void *p, const uint8_t *in, uint8_t *out) {
    auto &f = *(Fake *)p;
    f.encoded++;
    for (unsigned i = 0; i < 4; i++)
        out[i] = in[i];
    return true;
}
static bool send(void *p, const uint8_t *, size_t n, uint32_t frames) {
    auto &f = *(Fake *)p;
    assert(frames == n);
    assert(n % 4 == 0 && n <= f.plan.packet_bytes);
    f.sizes.push_back(n);
    return !f.fail_send;
}
static void stop(void *p) { ((Fake *)p)->stopped++; }
struct AbiFake {
    unsigned init = 0, encode = 0;
    uint64_t next_samples = 0;
    uint32_t rate = 0;
    unsigned packets = 0;
};
static int abi_init(void *s, short endian) {
    assert(endian == 0);
    ((AbiFake *)s)->init++;
    return 0;
}
static int abi_encode(void *s, void *l, void *r, void *out) {
    auto &f = *(AbiFake *)s;
    f.encode++;
    auto *left = (uint32_t *)l;
    auto *right = (uint32_t *)r;
    assert(left[0] == 0xff80 && right[0] == 0x807f);
    ((uint16_t *)out)[0] = 0x1234;
    ((uint16_t *)out)[1] = 0xabcd;
    return 0;
}
static bool transport_config(void *s, const Plan &p) {
    auto &f = *(AbiFake *)s;
    assert(p.wire[7] == 2 && p.aptx[7] == 1);
    f.rate = p.rate;
    return true;
}
static bool transport_send(void *s, const uint8_t *d, size_t n, uint32_t samples, uint64_t time) {
    auto &f = *(AbiFake *)s;
    assert(time == f.next_samples * 1000000 / f.rate);
    assert(d[0] == 0x12 && d[1] == 0x34 && d[2] == 0xab && d[3] == 0xcd);
    assert(n == samples);
    f.next_samples += samples;
    f.packets++;
    return true;
}
static void transport_stop(void *) {}
int main() {
    const uint8_t old_caps[] = {10, 0, 255, 10, 0, 0, 0, 2, 0, 0x32, 0};
    uint8_t ext[] = {19, 0, 255, 0xd7, 0, 0, 0, 2, 0, 0x32, 3, 0, 0xb4, 0, 0x68, 1, 50, 1, 0xb4, 0};
    Capabilities c{};
    assert(parse(ext, sizeof ext, &c) == Error::Ok && c.vendor == 0xd7 && c.target == 180 &&
           c.initial == 360 && c.working == 180 && c.backchannel);
    Plan p{};
    assert(negotiate(ext, sizeof ext, {48000, 16, false, 895}, &p) == Error::Ok);
    assert(p.wire_size == 20 && p.wire[3] == 0xd7 && p.wire[7] == 2 && p.wire[9] == 0x12 &&
           p.wire[10] == 2);
    assert(p.aptx[3] == 0x4f && p.aptx[7] == 1 && p.aptx[9] == 0x12 && p.packet_bytes == 360 &&
           p.frames_per_packet == 90);
    assert(negotiate(old_caps, sizeof old_caps, {44100, 16, false, 895}, &p) == Error::Ok &&
           p.wire_size == 11 && p.wire[3] == 10 && p.wire[9] == 0x22 && p.packet_bytes == 328);
    assert(negotiate(old_caps, sizeof old_caps, {48000, 16, false, 101}, &p) == Error::Ok &&
           p.packet_bytes == 100);
    Plan unchanged = p;
    assert(negotiate(ext, sizeof ext, {48000, 24, false, 895}, &p) == Error::Unsupported &&
           !memcmp(&p, &unchanged, sizeof p));
    assert(negotiate(ext, sizeof ext, {96000, 16, false, 895}, &p) == Error::Unsupported);
    assert(negotiate(ext, sizeof ext, {48000, 16, true, 895}, &p) == Error::Unsupported);
    assert(negotiate(ext, sizeof ext, {48000, 16, false, 3}, &p) == Error::Malformed);
    for (unsigned n = 0; n < 20; n++)
        assert(parse(ext, n, &c) != Error::Ok);
    for (unsigned n = 0; n < 11; n++)
        assert(parse(old_caps, n, &c) != Error::Ok);
    ext[10] = 0x80;
    assert(parse(ext, 20, &c) == Error::Malformed);
    ext[10] = 3;
    ext[11] = 1;
    assert(parse(ext, 20, &c) == Error::Malformed);
    ext[11] = 0;
    ext[3] = 0x4f;
    assert(parse(ext, 20, &c) == Error::Unsupported);
    ext[3] = 0xd7;
    ext[12] = ext[13] = ext[14] = ext[15] = ext[16] = ext[17] = ext[18] = ext[19] = 0;
    assert(negotiate(ext, 20, {48000, 16, false, 895}, &p) == Error::Ok && p.wire[12] == 180 &&
           p.wire[14] == 0x68 && p.wire[15] == 1 && p.wire[16] == 50 && p.wire[17] == 1);
    uint8_t a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {6, 5, 4, 3, 2, 1};
    Session session;
    assert(session.begin(a, {48000, 16, false, 895}) == Error::NoPeer);
    assert(session.discover(a, old_caps, sizeof old_caps) == Error::Ok);
    assert(!session.available(a));
    assert(session.begin(a, {48000, 16, false, 895}) == Error::BackendUnavailable);
    Fake f;
    assert(session.bind({&f, configure, encode, send, stop}) == Error::Ok);
    assert(session.available(a) && !session.available(b));
    assert(session.begin(b, {48000, 16, false, 895}) == Error::WrongPeer);
    assert(session.begin(a, {48000, 16, false, 895}) == Error::Ok);
    assert(session.discover(b, ext, 20) == Error::BackendFailure);
    uint8_t pcm[16 * 185]{};
    assert(session.encode(pcm, sizeof pcm) == Error::Ok && f.encoded == 185 &&
           f.sizes.size() == 2 && f.sizes[0] == 360 && f.sizes[1] == 360);
    assert(session.encode(pcm, 15) == Error::Malformed);
    assert(session.flush() == Error::Ok && f.sizes.back() == 20);
    f.fail_send = true;
    assert(session.encode(pcm, 16 * 90) == Error::BackendFailure && !session.active() &&
           f.stopped == 1);
    session.stop();
    assert(f.stopped == 1);
    session.reset();
    assert(!session.available(a) && session.begin(a, {48000, 16, false, 895}) == Error::NoPeer);
    // Exercise the actual classic-aptX ABI adapter and exact fractional pacing timestamps.
    AbiFake abi;
    ClassicBackend classic({&abi, abi_init, abi_encode},
                           {&abi, transport_config, transport_send, transport_stop});
    Session via_abi;
    assert(via_abi.discover(a, old_caps, sizeof old_caps) == Error::Ok);
    assert(via_abi.bind(classic.callbacks()) == Error::Ok);
    assert(via_abi.begin(a, {44100, 16, false, 895}) == Error::Ok);
    std::vector<uint8_t> audio(16 * 82 * 3000);
    for (size_t i = 0; i < audio.size(); i += 16) {
        audio[i] = 0x80;
        audio[i + 1] = 0xff;
        audio[i + 2] = 0x7f;
        audio[i + 3] = 0x80;
    }
    assert(via_abi.encode(audio.data(), audio.size()) == Error::Ok && abi.init == 1 &&
           abi.packets == 3000 && abi.next_samples == 328 * 3000);
    via_abi.stop();
    // Length-bounded hostile capability inputs; failed parses cannot mutate caller output.
    unsigned seed = 42;
    for (unsigned i = 0; i < 40000; i++) {
        uint8_t data[32];
        for (auto &v : data) {
            seed = seed * 1664525 + 1013904223;
            v = seed >> 24;
        }
        Capabilities before = c;
        Error error = parse(data, i % 33, &c);
        if (error != Error::Ok)
            assert(!memcmp(&before, &c, sizeof c));
    }
    puts("LL: both vendor IDs, legacy/extended CIEs, endian fields, strict rates/depth/duplex, "
         "MTU/pacing packet budget, per-peer state and encoder/send failure cleanup passed; 40000 "
         "malformed inputs bounded");
}
