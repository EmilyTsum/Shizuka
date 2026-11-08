// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace s25 {
constexpr size_t kWireCodecSize = 43;
struct State {
    uint8_t peer[kWireCodecSize];
    uint8_t selected[kWireCodecSize];
    uint32_t peer_seen;
    uint32_t selected_seen;
    uint32_t encoder_mode;
};
enum class Error { Ok = 0, Truncated = 1, Capacity = 2, Unsupported = 3, DuplicateCodec = 4 };
struct Result {
    Error error;
    size_t size;
    bool translated;
};
// Input/output are service-category TLVs; the LOSC byte is not on the wire.
Result incoming(const uint8_t *input, size_t size, uint8_t *output, size_t capacity, uint8_t signal,
                State *state);
Result outgoing(const uint8_t *input, size_t size, uint8_t *output, size_t capacity, uint8_t signal,
                State *state);
bool selection_valid(const uint8_t *codec, size_t size);
// QTI legacy controller command: 22-byte header plus 45-byte codec storage,
// with the reference implementation's final padding byte omitted from ParamLen.
size_t qti_start(const uint8_t *standard, size_t size, const State *, uint8_t *output,
                 size_t capacity);
} // namespace s25
