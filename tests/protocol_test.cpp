// SPDX-License-Identifier: GPL-2.0-only
#include "../src/protocol.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <random>
#include "../src/settings.h"
using namespace s25;
int main() {
    const uint8_t peer[] = {1,    0,    7,    42,   0,    255,  0xd7, 0, 0, 0,    0xad, 0, 0xd1,
                            0x0a, 0x53, 0x53, 0xfa, 0x7d, 0x64, 0x7d, 0, 1, 0x87, 0,    0, 0x0f,
                            2,    3,    3,    3,    0,    0xaa, 0,    0, 0, 0,    0,    0, 0,
                            0,    0,    0,    0,    0,    0,    0,    4, 2, 2,    0,    8, 0};
    const uint8_t hd[] = {7, 13, 0, 255, 0xd7, 0, 0, 0, 0x24, 0, 0x12, 0, 0, 0, 0};
    uint8_t proxy[] = {7, 13, 0, 255, 0xd7, 0, 0, 0, 0x24, 0, 0x12, 0xa5, 0xad, 255, 0};
    std::array<uint8_t, 512> out;
    out.fill(0xa5);
    State state{};
    auto r = outgoing(proxy, sizeof proxy, out.data(), out.size(), 3, &state);
    assert(r.error == Error::Unsupported);
    for (auto b : out)
        assert(b == 0xa5);
    r = incoming(peer, sizeof peer, out.data(), out.size(), 12, &state);
    assert(r.error == Error::Ok && r.translated && state.peer_seen && r.size == sizeof(peer) - 29);
    assert(out[2] == 7 && out[3] == 13 && out[10] == 0x24 && out[12] == 0x32);
    memcpy(proxy, out.data() + 2, 15);
    proxy[10] = 0x12;
    r = outgoing(proxy, sizeof proxy, out.data(), out.size(), 3, &state);
    assert(r.error == Error::Ok && r.size == 44 && state.selected_seen &&
           selection_valid(state.selected, 43));
    assert(state.selected[9] == kWireRate && state.selected[19] == kStream[1] &&
           state.selected[28] == kStream[10]);
    uint8_t start[57] = {1, 0x40};
    start[9] = 2;
    start[19] = 4;
    start[21] = 6;
    start[22] = 0x51;
    start[23] = 0x6f;
    start[24] = 3;
    uint8_t qti[68];
    memset(qti, 0xa5, sizeof qti);
    assert(qti_start(start, 57, &state, qti + 1, 66) == 66 && qti[0] == 0xa5 && qti[67] == 0xa5);
    assert(qti[1] == 10 && qti[2] == 10 && qti[4] == 10 && qti[16] == 0x7b && qti[17] == 3 &&
           qti[21] == 150);
    assert(!memcmp(qti + 23, state.selected, 43));
    assert(!qti_start(start, 56, &state, qti, 66) && !qti_start(start, 57, &state, qti, 65));
    std::array<uint8_t, 44> cfg;
    memcpy(cfg.data(), out.data(), 44);
    State receiving{};
    r = incoming(cfg.data(), 44, out.data(), 512, 3, &receiving);
    assert(r.error == Error::Ok && r.size == 15 && receiving.selected_seen);
    assert(out[0] == 7 && out[1] == 13 && out[8] == 0x24 && out[11] == 0xa5 && out[12] == 0xad);
    State before = state;
    out.fill(0xa5);
    r = incoming(peer, sizeof(peer), out.data(), 22, 12, &state);
    assert(r.error == Error::Capacity && memcmp(&state, &before, sizeof state) == 0);
    for (auto b : out)
        assert(b == 0xa5);
    r = incoming(hd, sizeof hd, out.data(), 512, 12, &state);
    assert(r.error == Error::Ok && !r.translated && r.size == sizeof hd &&
           !memcmp(out.data(), hd, sizeof hd));
    r = outgoing(hd, sizeof hd, out.data(), 512, 3, &state);
    assert(r.error == Error::Ok && !r.translated && r.size == sizeof hd &&
           !memcmp(out.data(), hd, sizeof hd));
    const uint8_t plain[] = {1, 0, 7, 6, 0, 0, 0x21, 0x15, 2, 53, 8, 0};
    r = incoming(plain, sizeof plain, out.data(), 512, 12, &state);
    assert(r.error == Error::Ok && !r.translated && r.size == sizeof plain &&
           memcmp(out.data(), plain, sizeof plain) == 0);
    std::mt19937 rng(0x931);
    for (unsigned test = 0; test < 40000; test++) {
        std::array<uint8_t, 128> in{};
        size_t n = rng() % 128;
        for (size_t i = 0; i < n; i++)
            in[i] = rng();
        if (test % 2 == 0) {
            n = sizeof(peer);
            memcpy(in.data(), peer, n);
            in[rng() % n] ^= rng();
            if (test % 3 == 0)
                n = rng() % n;
        }
        State s{}, saved = s;
        std::array<uint8_t, 514> guard;
        guard.fill(0xb7);
        size_t capacity = rng() % 513;
        Result x = incoming(in.data(), n, guard.data() + 1, capacity, 12, &s);
        assert(guard[0] == 0xb7);
        for (size_t i = capacity + 1; i < guard.size(); i++)
            assert(guard[i] == 0xb7);
        if (x.error != Error::Ok) {
            assert(memcmp(&s, &saved, sizeof s) == 0);
            for (auto b : guard)
                assert(b == 0xb7);
        } else
            assert(x.size <= capacity);
    }
    puts("protocol: Air6 capture, bidirectional configuration, bounds, unchanged fallback, and "
         "40000 malformed-input cases passed");
}
