// SPDX-License-Identifier: GPL-2.0-only
#include <initializer_list>

#include "../src/protocol.h"
#include "../src/coexist.h"
#include "../src/layout.h"
#include <jni.h>
#include <cassert>
#include <cstring>
#include <cstdio>
extern "C" void s25_anchor() {}
extern "C" jobject tramp_new_type(void *, int) { return nullptr; }
extern "C" jobjectArray tramp_supported(JNIEnv *, jobject) { return nullptr; }
extern "C" s25::Vector tramp_prepare(JNIEnv *, jobjectArray) { return {}; }
extern "C" uint8_t tramp_user(void *, const uint8_t *, const uint8_t *, const uint8_t *, bool,
                              uint8_t *, bool *, bool *, bool *) {
    return 0;
}
static int expected = 12;
static bool eligible = true;
extern "C" void tramp_callback(void *, uint8_t *cfg, s25::Vector *local, s25::Vector *selectable,
                               unsigned, unsigned) {
    assert(*(uint32_t *)cfg == unsigned(expected));
    assert(*(uint32_t *)(cfg + 12) == unsigned(expected == 12 ? 1 : 2));
    assert(*(uint64_t *)(cfg + 48) == 0);
    assert(local->end - local->begin == 128);
    assert(selectable->end - selectable->begin == (eligible ? 128 : 64));
    if (eligible) {
        auto *p = selectable->begin + 64;
        assert(*(uint32_t *)p == 12 && *(uint32_t *)(p + 8) == 1 && *(uint32_t *)(p + 12) == 1 &&
               *(uint64_t *)(p + 24) == 12288);
    }
}
extern "C" void s25_callback(void *, uint8_t *, s25::Vector *, s25::Vector *, unsigned, unsigned);
int main() {
    for (bool air : {true, false}) {
        uint8_t tlv[44] = {7, 42, 0, 255, 0xd7, 0, 0, 0, 0xad, 0};
        tlv[10] = air ? 0xd1 : 0x11;
        tlv[11] = 0x0a;
        tlv[19] = 1;
        tlv[20] = air ? 0x87 : 2;
        tlv[23] = 15;
        tlv[24] = 2;
        tlv[25] = tlv[26] = tlv[27] = 3;
        tlv[29] = 0xaa;
        s25::State state{};
        uint8_t output[64];
        auto r = s25::incoming(tlv, 44, output, 64, 12, &state);
        assert(r.error == s25::Error::Ok);
        uint8_t proxy[14] = {13};
        memcpy(proxy + 1, output + 2, 13);
        uint8_t flags;
        assert(s25::proxy_choose(proxy[12], 44100, 12288, &flags));
        s25::set_proxy(proxy, proxy[12], flags);
        alignas(16) uint8_t cfg[64]{};
        *(uint32_t *)cfg = 5;
        *(uint32_t *)(cfg + 12) = 2;
        *(uint64_t *)(cfg + 48) = s25::descriptor(proxy);
        uint8_t caps[64];
        memcpy(caps, cfg, 64);
        s25::Vector v{caps, caps + 64, caps + 64};
        eligible = air;
        expected = air ? 12 : 5;
        s25_callback(nullptr, cfg, &v, &v, 0, 0);
        assert(*(uint32_t *)cfg == 5 && *(uint32_t *)(cfg + 12) == 2 && v.end - v.begin == 64);
    }
    puts("callbacks: separate Lossless current/local/selectable types and CD16 display precision; "
         "non-R2.2 peer exclusion; source vectors unchanged");
}
