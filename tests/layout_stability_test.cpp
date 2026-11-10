// SPDX-License-Identifier: GPL-2.0-only
#include "../src/layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
    alignas(16) uint8_t cfg[128]{};
    *(uint32_t *)cfg = 9;
    *(uint32_t *)(cfg + 0x58) = 4;
    auto *a = (s25::AdaptiveConfig *)(cfg + 0x18);
    uint8_t stream[11] = {1, 0x97, 0, 0, 15, 2, 3, 3, 3, 0, 0xaa};
    a->sample_rate = 44100;
    a->channel_mode = 0;
    a->bits = 24;
    a->mode = 4096;
    a->fade_ms = 255;
    a->stream = {stream, stream + 11, stream + 11};
    for (unsigned i = 0; i < 6; i++)
        a->buffering[i] = i % 2 ? 50 : 20;
    a->buffering[2] = 60;
    a->buffering[3] = 100;
    assert(s25::adaptive_valid(cfg));
    a->buffering[2] = 61;
    assert(!s25::adaptive_valid(cfg));
    a->buffering[2] = 60;
    a->mode = 8192;
    assert(!s25::adaptive_valid(cfg));
    a->mode = 4096;
    a->sample_rate = 48000;
    assert(!s25::adaptive_valid(cfg));
    a->sample_rate = 44100;
    a->bits = 16;
    assert(!s25::adaptive_valid(cfg));
    a->bits = 24;
    stream[1] = 0x12;
    assert(!s25::adaptive_valid(cfg));
    stream[1] = 0x97;
    a->buffering[2] = 20;
    a->buffering[3] = 50;
    a->sample_rate = 96000;
    assert(s25::adaptive_valid(cfg));
    // Margin trial: minHQ buffering[2] = 100 (floor 110 ms) is accepted for
    // the same bounded 44.1/HQ profile; an out-of-envelope value is not.
    // (The blocks above rewrote stream[] and left sample_rate at 96000;
    // restore the 44.1/HQ state before re-entering the 44.1 checks.)
    stream[1] = 0x97;
    a->sample_rate = 44100;
    a->buffering[2] = 100;
    a->buffering[3] = 100;
    assert(s25::adaptive_valid(cfg));
    a->buffering[2] = 101;
    assert(!s25::adaptive_valid(cfg));
    puts("layout: bounded R2.2/44.1/HQ stability profile, minHQ margin-trial profile, "
         "unsupported depth/rate/mode rejection, standard Adaptive preserved");
}
