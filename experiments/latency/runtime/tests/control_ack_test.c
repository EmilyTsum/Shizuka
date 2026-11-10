/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/control_ack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void write32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static struct shizuka_control_ack_expect pending(void)
{
    struct shizuka_control_ack_expect e = {0};
    e.signal = 0x70001000;
    e.token = 0x6f7ab123;
    e.opcode = 0x01001000;
    e.source = 0x100abc;
    e.destination = 1;
    e.source_domain = 3;
    e.destination_domain = 2;
    e.sent = 1;
    return e;
}

static void response(uint8_t *p, const struct shizuka_control_ack_expect *e,
    uint32_t status)
{
    memset(p, 0, 32);
    write32(p, 0x2060);
    p[4] = e->source_domain;
    p[5] = e->destination_domain;
    write32(p + 8, e->destination);
    write32(p + 12, e->source);
    write32(p + 16, e->token);
    write32(p + 20, 0x02001005);
    write32(p + 24, e->opcode);
    write32(p + 28, status);
}

static int match(struct shizuka_control_ack_expect *e, const void *p, size_t n)
{
    return shizuka_control_ack_match(e, e->signal, 1, 0, p, n);
}

static int observe(struct shizuka_control_ack_expect *e, uint32_t flags,
    int32_t status, const void *p, size_t n)
{
    struct shizuka_control_ack_expect *list[] = {e};
    return shizuka_control_ack_observe_many(list, 1, e->signal, flags, status, p, n);
}

static void exact_wire_and_bounds(void)
{
    struct shizuka_control_ack_expect e = pending(), before = e;
    uint8_t p[32], copy[32], unaligned[33];
    unsigned byte, bit;
    response(p, &e, 0);
    memcpy(copy, p, sizeof(p));
    assert(match(&e, p, sizeof(p)) == 1);
    assert(!memcmp(&e, &before, sizeof(e)) && !memcmp(p, copy, sizeof(p)));
    memcpy(unaligned + 1, p, sizeof(p));
    assert(match(&e, unaligned + 1, 32) == 1);
    for (byte = 0; byte < sizeof(p); ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            memcpy(p, copy, sizeof(p));
            p[byte] ^= (uint8_t)(1u << bit);
            /* GPR client_data is opaque; every other bit is attested. */
            assert(match(&e, p, 32) == (byte == 6));
        }
    }
    assert(!match(&e, 0, 32));
    assert(!match(&e, (void *)(uintptr_t)1, 0));
    assert(!match(&e, (void *)(uintptr_t)1, 31));
    assert(!match(&e, (void *)(uintptr_t)1, 33));
    assert(!match(&e, (void *)(uintptr_t)1, SIZE_MAX));
    assert(!shizuka_control_ack_match(0, e.signal, 1, 0, copy, 32));
    assert(!shizuka_control_ack_match(&e, 0, 1, 0, copy, 32));
    assert(!shizuka_control_ack_match(&e, e.signal + 1, 1, 0, copy, 32));
    e.sent = 0;
    assert(!match(&e, copy, 32));
    e.sent = 2;
    assert(!match(&e, copy, 32));
}

static void full_token_and_alias(void)
{
    struct shizuka_control_ack_expect e = pending();
    uint8_t p[32];
    unsigned bit;
    response(p, &e, 0);
    for (bit = 0; bit < 32; ++bit) {
        write32(p + 16, e.token ^ (1u << bit));
        assert(!match(&e, p, 32));
    }
    write32(p + 16, e.token & 0xfff);
    assert(!match(&e, p, 32));
    response(p, &e, 0);
    assert(observe(&e, 1, 0, p, 32) == 1);
    /* Model outgoing/RSP allocator alias: GSL normalizes token after wake.
     * The already copied pre-wake result remains valid without retaining p. */
    write32(p + 16, e.token & 0xfff);
    assert(e.ack == SHIZUKA_CONTROL_ACK_SEEN && !e.ack_status);
    memset(p, 0xdd, sizeof(p));
    assert(e.ack == SHIZUKA_CONTROL_ACK_SEEN && !e.ack_status);
}

static void rejected_events_and_nack(void)
{
    struct shizuka_control_ack_expect e;
    uint8_t p[32];
    const uint32_t flags[] = {0, 2, 3, 0x10, 0x11, UINT32_MAX};
    size_t i;
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        e = pending(); response(p, &e, 0);
        assert(observe(&e, flags[i], 0, p, 32) == -1);
        assert(e.ack == SHIZUKA_CONTROL_ACK_INVALID);
    }
    e = pending(); response(p, &e, 0);
    assert(observe(&e, 1, 1, p, 32) == -1);
    assert(e.ack == SHIZUKA_CONTROL_ACK_INVALID);
    e = pending(); response(p, &e, 7);
    assert(observe(&e, 1, 7, p, 32) == 1);
    assert(e.ack == SHIZUKA_CONTROL_ACK_SEEN && e.ack_status == 7);
    e = pending(); response(p, &e, UINT32_MAX);
    assert(observe(&e, 1, -1, p, 32) == 1);
    assert(e.ack_status == UINT32_MAX);
    e = pending(); response(p, &e, 0);
    assert(observe(&e, 1, 0, 0, 0) == -1);
    assert(e.ack == SHIZUKA_CONTROL_ACK_INVALID);
    e = pending(); e.sent = 0;
    assert(observe(&e, 1, 0, p, 32) == -1);
    assert(e.ack == SHIZUKA_CONTROL_ACK_INVALID);
}

static void unique_signal_and_races(void)
{
    struct shizuka_control_ack_expect e[16], *list[16];
    uint8_t p[32];
    unsigned i;
    for (i = 0; i < 16; ++i) {
        e[i] = pending(); e[i].signal += i; e[i].source += i;
        list[i] = e + i;
    }
    response(p, e + 7, 0);
    assert(shizuka_control_ack_observe_many(list, 16, e[7].signal, 1, 0, p, 32) == 1);
    for (i = 0; i < 16; ++i)
        assert(e[i].ack == (i == 7 ? SHIZUKA_CONTROL_ACK_SEEN : SHIZUKA_CONTROL_ACK_PENDING));
    /* Duplicate exact ACK is rejected, even though original GSL may return 0. */
    assert(shizuka_control_ack_observe_many(list, 16, e[7].signal, 1, 0, p, 32) == -1);
    assert(e[7].ack == SHIZUKA_CONTROL_ACK_INVALID);
    /* Good then wrong-token overwrite, and wrong-token then good, both fail. */
    e[0] = pending(); response(p, e, 0);
    assert(observe(e, 1, 0, p, 32) == 1);
    write32(p + 16, e[0].token ^ 0x1000);
    assert(observe(e, 1, 0, p, 32) == -1);
    response(p, e, 0);
    assert(observe(e, 1, 0, p, 32) == -1);
    assert(e[0].ack == SHIZUKA_CONTROL_ACK_INVALID);
    /* Sharing the signal is ambiguous even when other tuples differ. */
    e[0] = pending(); e[1] = pending(); e[1].token++;
    response(p, e, 0);
    assert(shizuka_control_ack_observe_many(list, 2, e[0].signal, 1, 0, p, 32) == -1);
    assert(e[0].ack == SHIZUKA_CONTROL_ACK_INVALID && e[1].ack == SHIZUKA_CONTROL_ACK_INVALID);
    e[0] = pending();
    assert(shizuka_control_ack_observe_many(list, 1, e[0].signal + 99, 1, 0, p, 32) == 0);
    assert(!e[0].ack);
    list[1] = list[0];
    assert(shizuka_control_ack_observe_many(list, 2, e[0].signal, 1, 0, p, 32) == -1);
    assert(e[0].ack == SHIZUKA_CONTROL_ACK_INVALID);
    assert(shizuka_control_ack_observe_many(0, 0, 1, 1, 0, p, 32) == 0);
    assert(shizuka_control_ack_observe_many(0, 1, 1, 1, 0, p, 32) == -1);
    assert(shizuka_control_ack_observe_many(list, 17, 1, 1, 0, p, 32) == -1);
}

int main(void)
{
    exact_wire_and_bounds();
    full_token_and_alias();
    rejected_events_and_nack();
    unique_signal_and_races();
    puts("Control ACK: exact wire, full token, pre-wake alias, bounds, NACK and signal races passed");
    return 0;
}
