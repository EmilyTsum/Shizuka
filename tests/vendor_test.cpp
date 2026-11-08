// SPDX-License-Identifier: GPL-2.0-only
#include "../src/protocol.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../src/settings.h"
struct VscComplete {
    uint16_t opcode, length;
    uint8_t *bytes;
};
using Callback = void (*)(VscComplete *);
extern "C" void s25_vendor(uint16_t, uint8_t, uint8_t *, Callback);
extern "C" const s25::State *s25_debug_state();
extern "C" void s25_anchor() {}
extern "C" uint8_t tramp_prs_cfg(void *, uint8_t *, uint16_t, uint8_t *, uint8_t) { return 0; }
extern "C" void tramp_bld_cfg(uint8_t **, void *) {}
extern "C" bool tramp_to_hal(uint8_t *, void *) { return false; }
extern "C" bool tramp_enabled(const uint8_t *) { return false; }
extern "C" bool tramp_specific(void *, uint8_t *) { return false; }
static uint16_t opcode;
static uint8_t size, packet[256];
static Callback response;
extern "C" void tramp_vendor(uint16_t op, uint8_t n, uint8_t *p, Callback cb) {
    opcode = op;
    size = n;
    memcpy(packet, p, n);
    response = cb;
}
static int calls;
static uint8_t observed_status, observed_sub;
static void completed(VscComplete *p) {
    assert(p->opcode == 0xfd5d && p->length == 2);
    observed_status = p->bytes[0];
    observed_sub = p->bytes[1];
    calls++;
}
int main() {
    auto *s = const_cast<s25::State *>(s25_debug_state());
    s->selected_seen = 1;
    uint8_t cie[43] = {42, 0, 255, 0xd7, 0, 0, 0, 0xad, 0, 0x10, 8};
    cie[9] = s25::kWireRate;
    memcpy(s->selected, cie, 43);
    uint8_t command[57] = {1, 0x40};
    command[9] = 2;
    command[23] = 0x6f;
    command[24] = 3;
    s25_vendor(0xfd5d, 57, command, completed);
    assert(opcode == 0xfc0a && size == 66 && packet[1] == 10 && packet[15] == 0x7b);
    uint8_t status[2] = {0, 10};
    VscComplete event{0xfc0a, 2, status};
    response(&event);
    assert(calls == 1 && observed_status == 0 && observed_sub == 1);
    assert(opcode == 0xfc0a && size == 6 && packet[0] == 0x13 && packet[1] == 1 && packet[2] == 5 &&
           packet[3] == 2 && packet[4] == 0 && packet[5] == 0x10 && !response);
    uint8_t stop = 2;
    s25_vendor(0xfd5d, 1, &stop, completed);
    assert(opcode == 0xfc0a && size == 2 && packet[0] == 5);
    status[0] = 0x0c;
    response(&event);
    assert(calls == 2 && observed_status == 0x0c && observed_sub == 2);
    s25_vendor(0xfd5d, 1, &stop, completed);
    assert(opcode == 0xfd5d && size == 1 && response == completed);
    s25_vendor(0xfd5d, 57, command, completed);
    status[0] = 1;
    response(&event);
    assert(calls == 3 && observed_status == 1);
    s25_vendor(0xfd5d, 1, &stop, completed);
    assert(opcode == 0xfd5d);
    s->encoder_mode = 8192;
    s25_vendor(0xfd5d, 57, command, completed);
    status[0] = 0;
    response(&event);
    assert(packet[0] == 0x13 && packet[5] == 0x20 && calls == 4);
    s25_vendor(0xfd5d, 1, &stop, completed);
    response(&event);
    uint8_t other[2] = {8, 9};
    s25_vendor(0xfc17, 2, other, nullptr);
    assert(opcode == 0xfc17 && size == 2 && !memcmp(packet, other, 2) && !response);
    puts("vendor: QTI command bytes, preserved real success/error status, start/stop lifecycle, "
         "HQ/LL controller mode synchronization, and unchanged other commands passed");
}
