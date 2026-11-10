/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_CONTROL_ACK_H
#define SHIZUKA_CONTROL_ACK_H

#include <stddef.h>
#include <stdint.h>

#define SHIZUKA_CONTROL_ACK_MAX_WAITS 16u
#define SHIZUKA_CONTROL_ACK_PENDING 0u
#define SHIZUKA_CONTROL_ACK_SEEN 1u
#define SHIZUKA_CONTROL_ACK_INVALID 2u

struct shizuka_control_ack_expect {
    uintptr_t signal;
    uint32_t token, opcode, source, destination, sent, ack, ack_status;
    uint8_t source_domain, destination_domain;
};

/* Checks one complete, accessible 32-byte basic response without changing it.
 * The token is the full value captured immediately before async_send, after
 * GSL inserts its debug bits. Never normalize it to its lower 12 bits.
 * A matching nonzero status is a recorded NACK, not a positive ACK.
 */
int shizuka_control_ack_match(const struct shizuka_control_ack_expect *expect,
    uintptr_t signal, uint32_t event_flags, int32_t status,
    const void *packet, size_t available_bytes);

/* Caller holds the wait-table lock. Supply pointers to active waits only.
 * Records before the original signal_set can wake its waiter. Returns 0 for
 * an unrelated signal, 1 for a unique matching response, -1 for invalid input
 * or a rejected event. Every active wait sharing a nonunique signal becomes
 * invalid. A second event for the same signal also invalidates a seen ACK.
 * Release the lock before calling the original signal_set. Never use receiver
 * TID to identify a waiter, and never retain the packet pointer.
 */
int shizuka_control_ack_observe_many(
    struct shizuka_control_ack_expect *const *waits, size_t count,
    uintptr_t signal, uint32_t event_flags, int32_t status,
    const void *packet, size_t available_bytes);

#endif
