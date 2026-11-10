/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/control_ack.h"

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
        (uint32_t)p[3] << 24;
}

int shizuka_control_ack_match(const struct shizuka_control_ack_expect *expect,
    uintptr_t signal, uint32_t event_flags, int32_t status,
    const void *packet, size_t available_bytes)
{
    const uint8_t *p = packet;
    if (!expect || !signal || expect->signal != signal || expect->sent != 1 ||
        expect->ack != SHIZUKA_CONTROL_ACK_PENDING || event_flags != 1 ||
        !p || available_bytes != 32)
        return 0;
    return read32(p) == 0x2060 && p[7] == 0 &&
        read32(p + 20) == 0x02001005 && read32(p + 16) == expect->token &&
        read32(p + 8) == expect->destination &&
        read32(p + 12) == expect->source &&
        p[4] == expect->source_domain && p[5] == expect->destination_domain &&
        read32(p + 24) == expect->opcode && read32(p + 28) == (uint32_t)status;
}

int shizuka_control_ack_observe_many(
    struct shizuka_control_ack_expect *const *waits, size_t count,
    uintptr_t signal, uint32_t event_flags, int32_t status,
    const void *packet, size_t available_bytes)
{
    size_t i, matching = 0;
    struct shizuka_control_ack_expect *expect = 0;
    if (count > SHIZUKA_CONTROL_ACK_MAX_WAITS || (count && !waits))
        return -1;
    if (!signal)
        return 0;
    for (i = 0; i < count; ++i) {
        if (waits[i] && waits[i]->signal == signal) {
            ++matching;
            expect = waits[i];
        }
    }
    if (!matching)
        return 0;
    if (matching == 1 && shizuka_control_ack_match(expect, signal,
            event_flags, status, packet, available_bytes)) {
        expect->ack_status = (uint32_t)status;
        expect->ack = SHIZUKA_CONTROL_ACK_SEEN;
        return 1;
    }
    for (i = 0; i < count; ++i) {
        if (waits[i] && waits[i]->signal == signal)
            waits[i]->ack = SHIZUKA_CONTROL_ACK_INVALID;
    }
    return -1;
}
