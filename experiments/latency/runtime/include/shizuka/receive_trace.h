/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_RECEIVE_TRACE_H
#define SHIZUKA_RECEIVE_TRACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SHIZUKA_RECEIVE_TRACE_BYTES 56u
#define SHIZUKA_RECEIVE_TRACE_MAX_PACKET 1024u

/* Caller-owned zero-filled, immovable state. Only access through these helpers
 * while callbacks/readers can run. There is no retained packet pointer. */
struct shizuka_receive_trace {
    uint32_t writer;
    uint32_t sequence;
    uint8_t bytes[SHIZUKA_RECEIVE_TRACE_BYTES];
};

/* Record the last successfully captured GPR header plus at most 32 payload
 * bytes; zero the remainder for short packets. capacity is the actual readable
 * GPR-owned allocation (24..1024), not an unchecked packet_size claim. Require
 * version0/header24 and 24 <= declared packet_size <= capacity. No opcode or
 * domain policy is applied to this diagnostic prefix.
 * 1=captured, 0=try-lock busy/spurious failure, -1=invalid. Never waits, allocates,
 * frees, retains, sends, or changes the packet. A skipped record preserves the
 * last complete trace. Call before the existing receiver frees its packet. */
int shizuka_receive_trace_capture(struct shizuka_receive_trace *,
    const void *packet,size_t capacity);

/* Return 56 only for an initialized stable even sequence; otherwise return 0
 * and leave output unchanged. One bounded snapshot attempt; no lock or retry.
 * Output must have capacity >=56 and not alias state. */
size_t shizuka_receive_trace_snapshot(const struct shizuka_receive_trace *,
    void *output,size_t capacity);
#ifdef __cplusplus
}
static_assert(sizeof(shizuka_receive_trace)==64,"receive trace ABI");
#else
_Static_assert(sizeof(struct shizuka_receive_trace)==64,"receive trace ABI");
#endif
#endif
