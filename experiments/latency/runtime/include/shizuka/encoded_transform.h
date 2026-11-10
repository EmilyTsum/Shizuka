/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_ENCODED_TRANSFORM_H
#define SHIZUKA_ENCODED_TRANSFORM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SHZ_ENCODED_PROFILE_R3_HQ_44100 1U
#define SHZ_ENCODED_MAX_DATA (4U * 1024U * 1024U)
#define SHZ_ENCODED_MAX_METADATA (64U * 1024U)
#define SHZ_ENCODED_MAX_TTP_US 524280U

enum shz_encoded_status {
    SHZ_ENCODED_OK = 0,
    SHZ_ENCODED_INVALID_ARGUMENT = 1,
    SHZ_ENCODED_UNREVIEWED_PROFILE = 2,
    SHZ_ENCODED_INVALID_DELTA = 3,
    SHZ_ENCODED_INVALID_METADATA = 4,
    SHZ_ENCODED_INVALID_WRAPPER = 5,
    SHZ_ENCODED_TTP_OVERFLOW = 6
};

struct shz_encoded_result {
    uint64_t timestamp_bits;
    uint32_t wr_flags;
    uint32_t data_bytes;
    uint32_t metadata_bytes;
    uint32_t wrapper_count;
    uint32_t changed_wrapper_count;
    uint32_t timing_wrapper_count;
    uint32_t secondary_zero_count;
    uint32_t first_ttp_before_us;
    uint32_t first_ttp_after_us;
    uint32_t last_ttp_before_us;
    uint32_t last_ttp_after_us;
};

/* The owner must prove the codec/profile, mapping, epoch and CPU ownership.
 * Buffers and result must be accessible and nonoverlapping. Capacity describes
 * owned accessible memory, not a claimed mapping handle. No pointer is retained.
 * Validate the entire input first; errors leave both buffers/result unchanged.
 * Success rewrites only TTP words and compacts exactly one frame-info metadata
 * record out of metadata. Bytes beyond the returned metadata_bytes are stale
 * and must not be transmitted. Delta is the graph epoch's immutable value.
 * This function performs no allocation, file, DSP, GPR or cache operations. */
int shz_encoded_transform(uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
                          uint8_t *metadata, uint32_t metadata_bytes,
                          uint32_t metadata_capacity, uint64_t delta_us,
                          uint32_t profile, struct shz_encoded_result *result);

/* Same complete validation and output description, with no buffer mutation.
 * Used to attest the first actual R3 buffer before permitting a positive TTP. */
int shz_encoded_validate(const uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
    const uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity,
    uint64_t delta_us, uint32_t profile, struct shz_encoded_result *result);

#ifdef __cplusplus
}
#endif
#endif
