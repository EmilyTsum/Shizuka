// SPDX-License-Identifier: GPL-2.0-only
#include "../src/protocol.h"
#include "../src/coexist.h"
#include "../src/settings.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace s25;
static void cap(uint8_t *cie, bool b10) {
    memset(cie, 0, 43);
    cie[0] = 42;
    cie[2] = 255;
    cie[3] = 0xd7;
    cie[7] = 0xad;
    cie[9] = b10 ? 0x11 : 0xd1;
    cie[10] = 0x0a;
    const uint8_t ttp[6] = {0x53, 0x53, 0xfa, 0x7d, 0x64, 0x7d};
    memcpy(cie + 11, ttp, 6);
    cie[18] = 1;
    cie[19] = b10 ? 2 : 0x87;
    cie[22] = 0xf;
    cie[23] = 2;
    cie[24] = cie[25] = cie[26] = 3;
    cie[28] = 0xaa;
}
static void parsecap(const uint8_t *cie, uint8_t *proxy, State &s) {
    uint8_t tlv[44], out[512];
    tlv[0] = 7;
    tlv[1] = 42;
    memcpy(tlv + 2, cie + 1, 42);
    auto r = incoming(tlv, 44, out, sizeof out, 12, &s);
    assert(r.error == Error::Ok && r.size == 15);
    proxy[0] = 13;
    memcpy(proxy + 1, out + 2, 13);
    assert(proxy_codec(proxy));
}
int main() {
    assert(!flags_valid(8) && !flags_valid(9) && flags_valid(10));
    assert(!request_settings(96000, 16, kModeLossless));
    assert(request_settings(96000, 24, kModeLossless) && current_rate() == 44100 &&
           current_bits() == 24);
    assert(!request_settings(48000, 16, kModeHq));
    uint8_t air[43], b10[43], pa[14], pb[14];
    cap(air, false);
    cap(b10, true);
    State state{};
    assert(request_settings(96000, 24, 8192));
    parsecap(air, pa, state);
    parsecap(b10, pb, state);
    assert(pa[12] != pb[12] && proxy_caps(pa[12]) == 11 && proxy_caps(pb[12]) == 2);
    uint8_t a[43], b[43];
    assert(proxy_selection(pa, a) && proxy_selection(pb, b));
    assert(a[9] == 0xa0 && a[19] == 0x97 && b[9] == 0x10 && b[19] == 0x12);
    // Reversing the most recent peer must not change either saved proxy's negotiated configuration.
    parsecap(air, pa, state);
    assert(proxy_selection(pb, b) && b[9] == 0x10 && b[19] == 0x12);
    assert(proxy_selection(pa, a) && a[9] == 0xa0 && a[19] == 0x97);
    uint8_t flags;
    assert(proxy_choose(pa[12], 44100, 4096, &flags));
    set_proxy(pa, pa[12], flags);
    assert(proxy_selection(pa, a) && a[9] == 0x40 && flags_mode(pa[13]) == 4096);
    assert(proxy_choose(pb[12], 44100, 4096, &flags));
    set_proxy(pb, pb[12], flags);
    assert(proxy_selection(pb, b) && b[9] == 0x10 && flags_mode(pb[13]) == 4096);
    assert(proxy_choose(pa[12], 96000, kModeLossless, &flags));
    set_proxy(pa, pa[12], flags);
    assert(proxy_selection(pa, a) && a[9] == 0x40 && flags_rate(pa[13]) == 44100 &&
           flags_mode(pa[13]) == kModeLossless && a[13] == 0x64 && a[14] == 0x64 &&
           hal_mode(flags_mode(pa[13])) == 4096 && flags_bits(pa[13]) == 24);
    assert(proxy_choose(pb[12], 96000, kModeLossless, &flags));
    set_proxy(pb, pb[12], flags);
    assert(proxy_selection(pb, b) && b[9] == 0x10 && flags_rate(pb[13]) == 48000 &&
           flags_mode(pb[13]) == 4096);
    assert(proxy_choose(pa[12], 44100, 4096, &flags));
    set_proxy(pa, pa[12], flags);
    assert(proxy_r22(pa[12]) && !proxy_r22(pb[12]));
    pa[9] = pb[9] = 0x12;
    uint8_t packet[15] = {7, 13};
    memcpy(packet + 2, pa + 1, 13);
    uint8_t output[128];
    auto r = outgoing(packet, 15, output, sizeof output, 3, &state);
    assert(r.error == Error::Ok && state.selected[9] == 0x40);
    memcpy(packet + 2, pb + 1, 13);
    r = outgoing(packet, 15, output, sizeof output, 3, &state);
    assert(r.error == Error::Ok && state.selected[9] == 0x10 && state.selected[19] == 0x12);
    uint8_t start[57] = {1, 0x40}, command[66];
    start[9] = 8; // Deliberately incorrect proxy clock.
    memcpy(state.selected, a, 43);
    state.selected_seen = 1;
    assert(qti_start(start, 57, &state, command, 66) == 66 && command[9] == 1 &&
           command[20] == 150 && !command[21]);
    memcpy(state.selected, b, 43);
    assert(qti_start(start, 57, &state, command, 66) == 66 && command[9] == 2 &&
           command[20] == 150 && !command[21]);
    uint8_t bad[11];
    memcpy(bad, b + 18, 11);
    bad[1] |= 8;
    assert(!negotiated_stream_valid(bad));
    bad[1] = 0x32;
    assert(!negotiated_stream_valid(bad));
    puts("peer negotiation: independent Air6 R2.2 96/44.1 and BT-B10 R2.x 48, mode selection, "
         "interleaving and invalid feature rejection passed");
}
