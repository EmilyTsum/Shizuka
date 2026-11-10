/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_DATA_RECEIPT_TRACE_H
#define SHIZUKA_DATA_RECEIPT_TRACE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SHIZUKA_DATA_RECEIPT_TRACE_BYTES 512u
#define SHIZUKA_DATA_RECEIPT_TRACE_COUNT 8u
#define SHIZUKA_DATA_RECEIPT_TRACE_RECORD_BYTES 60u
#define SHIZUKA_DATA_RECEIPT_TRACE_MAX_PACKET 1024u

enum shizuka_data_receipt_trace_shape {
    SHIZUKA_DATA_RECEIPT_BAD_HEADER = 1u,
    SHIZUKA_DATA_RECEIPT_BAD_LENGTH = 2u,
    SHIZUKA_DATA_RECEIPT_BAD_DOMAIN = 4u,
    SHIZUKA_DATA_RECEIPT_BAD_PORT = 8u,
    SHIZUKA_DATA_RECEIPT_BAD_RESERVED = 16u,
    SHIZUKA_DATA_RECEIPT_TIME_OVERFLOW = 32u
};

/* Zero-initialized, immovable caller-owned storage. bytes MUST remain first:
 * a diagnostic reader may read exactly the first 512 bytes directly. All
 * numbers in bytes are LE, including the aligned sequence word at offset16.
 * Header: magic[8]="SHZDTR1\0", ABI32@8=1, size32@12=512,
 * sequence32@16, count32@20, epoch64@24. First eight records start at32.
 * Each60B record: receipt_ns64@0, token32@8, src32@12, dst32@16,
 * opcode32@20, status32@24, md_status32@28, data_bytes32@32,
 * md_bytes32@36, flags32@40, frames32@44, timestamp64@48, shape32@56.
 * BASIC has its returned command opcode in flags and zeros for data fields.
 * No audio, addresses, map handles, or packet pointers are retained.
 * The union exposes a properly typed sequence for atomic access. */
struct shizuka_data_receipt_trace {
    union {
        uint8_t bytes[SHIZUKA_DATA_RECEIPT_TRACE_BYTES];
        struct {
            uint8_t before_sequence[16];
            uint32_t sequence;
            uint8_t after_sequence[492];
        } wire;
    };
    uint32_t writer;
};

/* One nonblocking try-lock; never allocates, retains, waits, sends or frees.
 * Record RD_DONE05001005(total80), WR_DONE05001004(total56), negative
 * BASIC02001005(total32), or malformed selected responses. Valid positive
 * BASIC and unrelated opcodes are ignored. Expected incoming domains2->3,
 * RD43f7/WR43fa -> client535a6101; BASIC also permits APM1. Shape errors are
 * diagnostic only, NEVER proof of transport authorization. Caller supplies
 * the actually accessible GPR-owned capacity, not an unchecked wire size.
 * Nonzero epochs must increase; stale epochs cannot erase a newer record.
 * time_ms is converted to receipt_ns; overflow saturates and sets shape.
 * Returns1=captured,0=busy/filtered/full/stale,-1=invalid caller bounds. */
int shizuka_data_receipt_trace_capture(struct shizuka_data_receipt_trace *,
    uint64_t epoch,const void *packet,size_t capacity,uint64_t time_ms);

/* One sequence-coherent snapshot attempt, no retry/lock. Return512 only for
 * initialized stable even sequence; otherwise0, output unchanged. Output
 * must not overlap the state. External raw readers must likewise compare
 * sequence@16 before and after their 512B read. */
size_t shizuka_data_receipt_trace_snapshot(const struct shizuka_data_receipt_trace *,
    void *output,size_t capacity);

#ifdef __cplusplus
}
static_assert(offsetof(shizuka_data_receipt_trace,bytes)==0,"sidecar first");
static_assert(offsetof(shizuka_data_receipt_trace,writer)==512,"sidecar extent");
static_assert(offsetof(shizuka_data_receipt_trace,wire.sequence)==16,"sidecar sequence");
#else
_Static_assert(offsetof(struct shizuka_data_receipt_trace,bytes)==0,"sidecar first");
_Static_assert(offsetof(struct shizuka_data_receipt_trace,writer)==512,"sidecar extent");
_Static_assert(offsetof(struct shizuka_data_receipt_trace,wire.sequence)==16,"sidecar sequence");
#endif
#endif
