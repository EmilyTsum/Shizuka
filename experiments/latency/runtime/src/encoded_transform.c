/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/encoded_transform.h"

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static int overlaps(const void *a, uint32_t an, const void *b, uint32_t bn) {
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (ap > UINTPTR_MAX - an || bp > UINTPTR_MAX - bn) return 1;
    return an && bn && ap < bp + bn && bp < ap + an;
}
static uint32_t wrapper_bytes(uint32_t type) {
    static const uint16_t payload[9] = {348, 656, 140, 152, 560, 760, 960, 348, 980};
    return 8U + payload[type];
}
static void move_forward(uint8_t *dst, const uint8_t *src, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; ++i) dst[i] = src[i];
}
static int encoded_run(uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
                          uint8_t *metadata, uint32_t metadata_bytes,
                          uint32_t metadata_capacity, uint64_t delta_us,
                          uint32_t profile, struct shz_encoded_result *result, int commit) {
    uint32_t cursor, frame_count = 0, ts_low = 0, ts_high = 0, ts_flags = 0;
    uint32_t wrappers = 0, changed = 0, timing = 0, secondary = 0;
    uint32_t first_before = 0, first_after = 0, last_before = 0, last_after = 0;
    uint32_t frame_at = 0, output_md = 0;
    if (!data || !metadata || !result || ((uintptr_t)result % _Alignof(struct shz_encoded_result)) || !data_bytes ||
        data_bytes > SHZ_ENCODED_MAX_DATA || metadata_bytes > SHZ_ENCODED_MAX_METADATA ||
        data_bytes > data_capacity || metadata_bytes > metadata_capacity ||
        overlaps(data, data_bytes, metadata, metadata_bytes) ||
        overlaps(data, data_bytes, result, (uint32_t)sizeof(*result)) ||
        overlaps(metadata, metadata_bytes, result, (uint32_t)sizeof(*result)))
        return SHZ_ENCODED_INVALID_ARGUMENT;
    if (profile != SHZ_ENCODED_PROFILE_R3_HQ_44100) return SHZ_ENCODED_UNREVIEWED_PROFILE;
    if (delta_us > SHZ_ENCODED_MAX_TTP_US || delta_us % 8U) return SHZ_ENCODED_INVALID_DELTA;

    /* Pass one: every metadata record must be reviewed and fully bounded. */
    cursor = 0;
    while (cursor < metadata_bytes) {
        uint32_t id, flags, offset, low, high, size, expected;
        if (metadata_bytes - cursor < 24U) return SHZ_ENCODED_INVALID_METADATA;
        id = read32(metadata + cursor); flags = read32(metadata + cursor + 4U);
        offset = read32(metadata + cursor + 8U); low = read32(metadata + cursor + 12U);
        high = read32(metadata + cursor + 16U); size = read32(metadata + cursor + 20U);
        if (id == 0x0a001052U) expected = 16U;
        else if (id == 0x0a001037U || id == 0x0a001039U) expected = 4U;
        else if (id == 0x0a001038U) expected = 40U;
        else return SHZ_ENCODED_INVALID_METADATA;
        if ((flags & ~0x7fU) || (flags & 0x40U) || (flags & 0x0eU) || low || high ||
            size % 4U || size != expected || size > metadata_bytes - cursor - 24U || offset > data_bytes)
            return SHZ_ENCODED_INVALID_METADATA;
        if (id == 0x0a001052U) {
            if (++frame_count != 1U || offset || read32(metadata + cursor + 24U) != data_bytes)
                return SHZ_ENCODED_INVALID_METADATA;
            ts_low = read32(metadata + cursor + 28U); ts_high = read32(metadata + cursor + 32U);
            ts_flags = read32(metadata + cursor + 36U);
            if (ts_flags & ~0x80000000U) return SHZ_ENCODED_INVALID_METADATA;
            frame_at = cursor;
        }
        cursor += 24U + size;
    }
    if (frame_count != 1U) return SHZ_ENCODED_INVALID_METADATA;

    /* Pass two: prove every contiguous wrapper and every addition first. */
    cursor = 0;
    while (cursor < data_bytes) {
        uint32_t type, flags, size, before, after;
        if (data_bytes - cursor < 8U) return SHZ_ENCODED_INVALID_WRAPPER;
        type = data[cursor + 3U]; flags = data[cursor + 4U];
        if (type >= 9U || (flags != 0 && flags != 0x80U && flags != 0xa0U && flags != 0xc0U) ||
            data[cursor + 5U] || data[cursor + 6U] || !data[cursor + 2U] || data[cursor + 2U] % 4U ||
            (data[cursor + 7U] != 0xadU && data[cursor + 7U] != 0xaeU && data[cursor + 7U] != 0xafU))
            return SHZ_ENCODED_INVALID_WRAPPER;
        size = wrapper_bytes(type);
        if (size > data_bytes - cursor) return SHZ_ENCODED_INVALID_WRAPPER;
        before = read16(data + cursor);
        if ((flags == 0xc0U && before) || (flags != 0xc0U && delta_us && !before))
            return SHZ_ENCODED_INVALID_WRAPPER;
        after = flags == 0xc0U ? 0U : before + (uint32_t)(delta_us / 8U);
        if (after > 65535U) return SHZ_ENCODED_TTP_OVERFLOW;
        ++wrappers; changed += before != after;
        if (flags == 0xc0U) ++secondary;
        else {
            if (!timing) { first_before = before * 8U; first_after = after * 8U; }
            ++timing; last_before = before * 8U; last_after = after * 8U;
        }
        cursor += size;
    }

    /* All checks passed. The owner must keep exclusive CPU ownership throughout
     * this call; concurrent mutation is outside the buffer contract. */
    cursor = 0;
    while (commit && cursor < data_bytes) {
        uint32_t type = data[cursor + 3U];
        if (data[cursor + 4U] != 0xc0U)
            write16(data + cursor, (uint16_t)(read16(data + cursor) + delta_us / 8U));
        cursor += wrapper_bytes(type);
    }
    output_md = metadata_bytes - 40U;
    if (commit) move_forward(metadata + frame_at, metadata + frame_at + 40U, metadata_bytes - frame_at - 40U);
    result->timestamp_bits = (uint64_t)ts_low | ((uint64_t)ts_high << 32);
    result->wr_flags = 0x10U | ts_flags;
    result->data_bytes = data_bytes; result->metadata_bytes = output_md;
    result->wrapper_count = wrappers; result->changed_wrapper_count = changed;
    result->timing_wrapper_count = timing; result->secondary_zero_count = secondary;
    result->first_ttp_before_us = first_before; result->first_ttp_after_us = first_after;
    result->last_ttp_before_us = last_before; result->last_ttp_after_us = last_after;
    return SHZ_ENCODED_OK;
}

int shz_encoded_transform(uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
    uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity,
    uint64_t delta_us, uint32_t profile, struct shz_encoded_result *result) {
    return encoded_run(data,data_bytes,data_capacity,metadata,metadata_bytes,
                       metadata_capacity,delta_us,profile,result,1);
}
int shz_encoded_validate(const uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
    const uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity,
    uint64_t delta_us, uint32_t profile, struct shz_encoded_result *result) {
    return encoded_run((uint8_t *)data,data_bytes,data_capacity,(uint8_t *)metadata,
                       metadata_bytes,metadata_capacity,delta_us,profile,result,0);
}
