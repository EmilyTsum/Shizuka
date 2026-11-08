// SPDX-License-Identifier: GPL-2.0-only
#include "../src/aptx_ll_native.h"
#include <cassert>
#include <cstring>
#include <cstdio>
extern "C" void s25_anchor() {}
using namespace s25::ll;
int main() {
    uint8_t caps[] = {1, 0, 7, 19, 0, 255, 0xd7, 0, 0, 0, 2, 0, 0x32, 2, 0,
                      0, 0, 0, 0,  0, 0,   0,    0, 4, 2, 2, 0, 8,    0};
    uint8_t converted[128]{}, wire[128]{};
    size_t n = 0, m = 0;
    assert(translate(caps, sizeof caps, converted, sizeof converted, &n, false, 12));
    assert(n == sizeof caps - 6);
    uint8_t proxy_cie[20]{};
    proxy_cie[0] = converted[3];
    memcpy(proxy_cie + 1, converted + 4, 13);
    assert(proxy(proxy_cie));
    uint8_t slot = proxy_cie[12];
    assert(capabilities(slot) == 3 && !selection_slot(slot));
    proxy_cie[9] = 0x12;
    proxy_cie[13] = 2;
    memcpy(converted + 4, proxy_cie + 1, 13);
    assert(translate(converted, n, wire, sizeof wire, &m, true, 3));
    bool media = false, protect = false;
    for (size_t p = 0; p < m; p += wire[p + 1] + 2) {
        assert(wire[p] != 8);
        if (wire[p] == 7) {
            media = true;
            assert(wire[p + 1] == 19 && wire[p + 4] == 0xd7 && wire[p + 8] == 2 &&
                   wire[p + 10] == 0x12);
        }
        if (wire[p] == 4)
            protect = true;
    }
    assert(media && protect);
    // Delay reporting must remain untouched for ordinary aptX and all other codecs.
    uint8_t normal[] = {1, 0, 7, 9, 0, 255, 0x4f, 0, 0, 0, 1, 0, 0x22, 8, 0};
    assert(translate(normal, sizeof normal, wire, sizeof wire, &m, true, 3) && m == sizeof normal &&
           !memcmp(wire, normal, m));
    size_t sentinel = 999;
    assert(!translate(caps, sizeof caps - 1, wire, sizeof wire, &sentinel, false, 12) &&
           sentinel == 999);
    assert(!translate(caps, sizeof caps, wire, 2, &sentinel, false, 12));
    alignas(16) uint8_t codec[0x1e0]{};
    uint8_t input[20] = {9, 0, 255, 0x4f, 0, 0, 0, 1, 0, 0x22}, chosen[20]{};
    assert(!user_peer(codec, input, chosen));
    *(uint64_t *)(codec + 0x110) = kDescriptor | (uint64_t(slot) << 8);
    assert(user_peer(codec, input, chosen) && proxy(chosen) && chosen[12] == slot &&
           chosen[13] == 0);
    // A second endpoint/vendor cannot replace the capability recorded in this object.
    caps[6] = 0x0a;
    assert(translate(caps, sizeof caps, converted, sizeof converted, &n, false, 12));
    assert(user_peer(codec, input, chosen) && chosen[12] == slot);
    uint8_t csr[20]{};
    csr[0] = converted[3];
    memcpy(csr + 1, converted + 4, 13);
    alignas(16) uint8_t context[0x450]{};
    context[0x3e7] = 2;
    uint8_t *a = context + 6, *b = context + 32;
    a[0] = 7;
    a[1] = 8;
    memcpy(a + 2, proxy_cie, 20);
    b[0] = 5;
    b[1] = 6;
    memcpy(b + 2, csr, 20);
    memcpy(context + 0x400, proxy_cie, 20);
    *(uint8_t **)(context + 0x3f0) = a;
    uint8_t index = 255;
    assert(endpoint_index(context, &index) && index == 7);
    prioritize_peer(context);
    assert(context[7] == 6 && context[33] == 8 && *(uint8_t **)(context + 0x3f0) == b);
    assert(endpoint_index(context, &index) && index == 7);
    csr[9] = 0x12;
    csr[13] = 2;
    memcpy(context + 0x400, csr, 20);
    assert(endpoint_index(context, &index) && index == 5);
    context[0x3e7] = 20;
    index = 255;
    assert(!endpoint_index(context, &index) && index == 255);
    prioritize_peer(context);
    puts("LL native protocol: real Motion+ CIE translation, per-object capabilities, LL-only Delay "
         "Reporting removal, ordinary codec passthrough, alias-aware SEP lookup/order/pointer "
         "preservation and bounds passed");
}
