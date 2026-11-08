// SPDX-License-Identifier: GPL-2.0-only
// Host-only stand-ins. Native AArch64 builds receive real hash-gated trampolines.
#include <stdint.h>
extern "C" void tramp_ll_selectable(void *, void *) { __builtin_trap(); }
extern "C" uint8_t tramp_ll_sep_index(const uint8_t *) { __builtin_trap(); }
