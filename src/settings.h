// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#ifndef S25_RATE
#define S25_RATE 96000
#endif
#ifndef S25_MODE
#define S25_MODE 8192
#endif
#ifndef S25_R2
#define S25_R2 1
#endif
namespace s25 {
int proxy_rate();
int current_rate();
int current_bits();
int current_mode();
// Explicit user retries need a new private clock even if the desired values match.
bool request_settings(int rate, int bits, int mode, bool reconfigure = false);
bool consume_settings_change();
constexpr int kRate = S25_RATE, kMode = S25_MODE, kR2 = S25_R2;
constexpr uint8_t kWireRate = kRate == 96000 ? 0xa0 : kRate == 44100 ? 0x40 : 0x10;
constexpr uint8_t kStream[11] = {kR2 ? uint8_t(1) : uint8_t(0),
                                 kR2 ? uint8_t(kR2 == 3   ? 0x97
                                               : kR2 == 2 ? 0x83
                                                          : 0x80)
                                     : uint8_t(0xaa),
                                 0,
                                 0,
                                 kR2 ? uint8_t(0xf) : uint8_t(0),
                                 kR2 ? uint8_t(2) : uint8_t(0),
                                 kR2 ? uint8_t(3) : uint8_t(0),
                                 kR2 ? uint8_t(3) : uint8_t(0),
                                 kR2 ? uint8_t(3) : uint8_t(0),
                                 0,
                                 kR2 ? uint8_t(0xaa) : uint8_t(0)};
} // namespace s25
