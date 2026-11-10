// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>

// Same-process APSS attestation. This is never serialized to PAL, GPR, or the DSP.
#define S25_LATENCY_CONTEXT_MAGIC UINT32_C(0x31585a53) /* SZX1 */
#define S25_LATENCY_CONTEXT_ABI UINT32_C(1)
#define S25_LATENCY_TAG_MAGIC UINT32_C(0x315a4853) /* SHZ1 */
#define S25_LATENCY_STREAM_BYTES 11u
#define S25_LATENCY_TAGGED_BYTES 27u

typedef struct S25LatencyContext {
    uint32_t magic;
    uint32_t abi;
    uint32_t size;
    uint32_t sequence; // zero/odd is unpublished; readers must recheck after copying.
    uint32_t valid;
    uint32_t sample_rate;
    uint32_t mode; // 4096 HQ, 8192 LL, 12288 Lossless, before HAL mode collapse.
    uint32_t private_flags;
    uint32_t config_fingerprint;
    uint32_t reserved[7];
} S25LatencyContext;

#ifdef __cplusplus
extern "C" {
#endif
extern S25LatencyContext s25_latency_context;
// Bounded seqlock snapshot. Returns false for an unpublished/inconsistent record.
int s25_latency_context_snapshot(S25LatencyContext *out);
#ifdef __cplusplus
}
static_assert(sizeof(S25LatencyContext) == 64);
static_assert(offsetof(S25LatencyContext, sequence) == 12);
static_assert(offsetof(S25LatencyContext, config_fingerprint) == 32);
namespace s25 {
// Both fingerprints and the tag checksum use FNV-1a with explicit LE32 fields.
uint32_t latency_config_fingerprint(const uint8_t *cfg);
bool latency_tag_write(uint8_t *cfg, uint8_t private_flags);
bool latency_tag_valid(const uint8_t *cfg);
// Accept legacy 11-byte configs, but attest only a verified 27-byte input.
// Always strips a structurally readable 27-byte Adaptive vector before returning,
// even when its tag is rejected; capacity stays unchanged for its owning destructor.
bool latency_validate_and_capture(uint8_t session, uint8_t *cfg);
void latency_context_invalidate();
} // namespace s25
#else
_Static_assert(sizeof(S25LatencyContext) == 64, "latency context ABI");
_Static_assert(offsetof(S25LatencyContext, sequence) == 12, "latency seqlock ABI");
#endif
