// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace s25 {
// Verified for this DZIF's Samsung generated AIDL ABI; not the standard AIDL enum.
struct Vector {
    uint8_t *begin;
    uint8_t *end;
    uint8_t *capacity;
};
struct AdaptiveConfig {
    int32_t sample_rate;
    int32_t channel_mode;
    uint8_t bits;
    uint8_t padding[3];
    int32_t mode;
    uint8_t buffering[6];
    int8_t ttp[6];
    int32_t input_mode;
    int32_t fade_ms;
    uint8_t align[4];
    Vector stream;
};
static_assert(offsetof(AdaptiveConfig, stream) == 40);
static_assert(sizeof(AdaptiveConfig) == 64);
bool adaptive_valid(const uint8_t *config);
} // namespace s25
