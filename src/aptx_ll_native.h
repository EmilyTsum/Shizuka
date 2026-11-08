// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "aptx_ll.h"
namespace s25::ll {
constexpr int kPrivateIndex = 9; // Unimplemented source slot, before Samsung sink indices13+.
inline bool proxy(const uint8_t *c) {
    return c && c[0] == 13 && !c[1] && c[2] == 255 && c[3] == 0xd7 && !c[4] && !c[5] && !c[6] &&
           c[7] == 0x24 && !c[8] && c[10] == 0xa5 && c[11] == 0x4c &&
           (c[12] < 64 || c[12] == 255) && (c[13] == 0 || c[13] == 1 || c[13] == 2);
}
unsigned capabilities(uint8_t slot);
bool selection_slot(uint8_t slot);
bool make_wire(const uint8_t *proxy, uint8_t *out, size_t *size);
void normal_cie(const uint8_t *proxy, uint8_t *out20);
bool translate(const uint8_t *in, size_t size, uint8_t *out, size_t capacity, size_t *out_size,
               bool outgoing, uint8_t signal);
bool user_peer(void *codec, const uint8_t *peer, uint8_t *out20);
void fix_object(void *codec);
void *factory(int priority);
bool hal_view(void *codec, uint8_t *out, bool (*convert)(uint8_t *, void *));
bool specific_view(void *codec, uint8_t *out, bool (*convert)(void *, uint8_t *));
void prioritize_peer(void *peer);
bool endpoint_index(const void *peer, uint8_t *out_index);
void deactivate();
bool limit_hal(uint8_t *config);
bool offload_start(const uint8_t *in, size_t size, uint8_t *out);
} // namespace s25::ll
