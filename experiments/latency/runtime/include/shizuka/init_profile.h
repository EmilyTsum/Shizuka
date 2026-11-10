/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_INIT_PROFILE_H
#define SHIZUKA_INIT_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#include "../../../../../src/latency_context.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SHIZUKA_ADAPTIVE_INIT_BYTES 59u
struct shizuka_init_profile_fields {
    uint32_t raw_mode;
    uint32_t sample_rate_enum;
    uint8_t sink_capability_0,sink_capability_1;
    uint8_t reserved[2];
};
/* Caller owns a readable APSS payload/context snapshot and has separately
 * matched a successful original INIT ACK for the exact encoder/graph epoch.
 * Exact59B INIT uses rateenum@0, profile@36, sinkCapability@48 (11B).
 * Samsung R2.1 packing augments profile with cap[0]<<8 | cap[1]. Accept only
 * the exact augmented HQ form and reviewed R3 capability plus true private
 * non-Lossless HQ context. A pure0x1000 profile is deliberately not admitted.
 *
 * Return1=eligible,0=reject. Optional diagnostics contain the ACTUAL raw fields
 * for any bounded59B input, including rejected profiles; invalid bounds leave
 * output unchanged. Diagnostics must not overlap input/context. No mutation of
 * input/context, allocation, imports or I/O. This proves INIT capability/mode,
 * not actual R3 framing; the first real buffer still needs the strict parser.
 */
int shizuka_init_profile_hq_r3(const void *payload,size_t payload_size,
    size_t readable_capacity,const S25LatencyContext *context,
    struct shizuka_init_profile_fields *optional_diagnostics);
#ifdef __cplusplus
}
#endif
#endif
