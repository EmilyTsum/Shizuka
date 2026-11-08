// SPDX-License-Identifier: GPL-2.0-only
#ifndef S25_HAL_ONLY
#include "coexist.h"
#include "aptx_ll_native.h"
#include "settings.h"
#include <stddef.h>
extern "C" __attribute__((visibility("hidden"))) void s25_anchor();
extern "C" void *tramp_factory(int, int);
extern "C" bool tramp_supported_ss(int);
extern "C" int tramp_source_index(const uint8_t *);
extern "C" int tramp_vendor_source_index(const uint8_t *);
extern "C" uint64_t tramp_provider_index(const uint8_t *);
extern "C" bool tramp_init_codec(int, void *);
extern "C" void *memcpy(void *, const void *, size_t);
static uintptr_t base() { return (uintptr_t)&s25_anchor - 0x2000000; }
static uintptr_t adaptive_vtable[13];
static uint8_t vlock;
static void fix(void *obj) {
    auto *p = (uint8_t *)obj;
    *(uint32_t *)(p + 0x30) = s25::kPrivateAdaptive;
    *(uint64_t *)(p + 0x38) = 0xad00d7ff;
    const char name[] = "aptX Adaptive";
    p[0x40] = 26;
    for (unsigned i = 0; i < sizeof name; i++)
        p[0x41 + i] = name[i];
    for (unsigned off = 0x60; off <= 0x160; off += 0x40)
        *(uint32_t *)(p + off) = s25::kPrivateAdaptive;
    for (unsigned off = 0x1a0; off <= 0x1c8; off += 20)
        s25::mark_proxy(p + off);
}
extern "C" uint8_t s25_adaptive_set(void *obj, const uint8_t *peer, bool capability, uint8_t *out) {
    if (!s25::proxy_codec(peer))
        return 0x29;
    auto *p = (uint8_t *)obj;
    uint8_t flags;
    uint64_t saved_caps = *(uint64_t *)(p + 0x110);
    uint8_t cap_slot = peer[12];
    if (s25::proxy_is_selection(cap_slot) && s25::descriptor_valid(saved_caps) &&
        s25::proxy_caps((saved_caps >> 8) & 255) &&
        !s25::proxy_is_selection((saved_caps >> 8) & 255))
        cap_slot = (saved_caps >> 8) & 255;
    uint64_t request = *(uint64_t *)(p + 0x150);
    bool explicit_request = (request & 0xffffffff00000000ULL) == s25::kRequestDescriptor &&
                            s25::flags_valid(request & 15);
    int rate = explicit_request ? s25::flags_rate(request & 15) : s25::current_rate();
    int mode = explicit_request ? s25::flags_mode(request & 15) : s25::current_mode();
    if (explicit_request && (request & 0x100)) {
        unsigned want = rate == 96000 ? 8 : rate == 44100 ? 1 : 2;
        if (!(s25::proxy_caps(cap_slot) & want))
            return 0xc4;
    }
    if (explicit_request && mode == s25::kModeLossless &&
        (!(s25::proxy_caps(cap_slot) & 1) || !s25::proxy_r22(cap_slot)))
        return 0xc4;
    if (!s25::proxy_choose((capability || explicit_request) ? cap_slot : peer[12], rate, mode,
                           &flags))
        return 0x29;
    auto original = (uint8_t (*)(void *, const uint8_t *, bool, uint8_t *))(base() + 0x977290);
    uint8_t result = original(obj, peer, capability, out);
    fix(obj);
    if (result == 0) {
        if (out)
            s25::set_proxy(out, cap_slot, flags);
        s25::set_proxy(p + 0x1a0, cap_slot, flags);
        *(uint64_t *)(p + 0x90) = s25::descriptor(p + 0x1a0);
        *(uint64_t *)(p + 0x78) = s25::flags_mode(flags);
        *(uint64_t *)(p + 0x110) = s25::kDescriptor | (uint64_t(cap_slot) << 8);
    }
    return result;
}
extern "C" bool s25_adaptive_peer(void *obj, const uint8_t *peer) {
    if (!s25::proxy_codec(peer))
        return false;
    auto original = (bool (*)(void *, const uint8_t *))(base() + 0x9777b4);
    uint64_t saved = *(uint64_t *)((uint8_t *)obj + 0x110);
    bool result = original(obj, peer);
    fix(obj);
    if (result) {
        bool retain = s25::proxy_is_selection(peer[12]) && s25::descriptor_valid(saved) &&
                      s25::proxy_caps((saved >> 8) & 255) &&
                      !s25::proxy_is_selection((saved >> 8) & 255);
        *(uint64_t *)((uint8_t *)obj + 0x110) = retain ? saved : s25::descriptor(peer);
    }
    return result;
}
extern "C" void *s25_factory(int index, int priority) {
    if (index == s25::ll::kPrivateIndex)
        return s25::ll::factory(priority);
    if (index != s25::kPrivateAdaptive)
        return tramp_factory(index, priority);
    {
        char text[128];
        auto prop = (int (*)(const char *, char *, const char *))(base() + 0xdf4e84);
        prop("bluetooth.a2dp.source.aptx_adaptive_priority.config", text, "6001");
        unsigned v = 0;
        for (unsigned i = 0; text[i]; i++) {
            if (i >= 6 || text[i] < '0' || text[i] > '9')
                return nullptr;
            v = v * 10 + (text[i] - '0');
        }
        priority = (int)v;
    }
    auto allocate = (void *(*)(size_t))(base() + 0xe62580);
    void *obj = allocate(0x1e0);
    if (!obj)
        return nullptr;
    ((void (*)(void *, int))(base() + 0x9771c0))(obj, priority);
    while (__atomic_test_and_set(&vlock, __ATOMIC_ACQUIRE)) {
    }
    if (!adaptive_vtable[2]) {
        auto *old = *(uintptr_t **)obj;
        for (int i = -2; i < 11; i++)
            adaptive_vtable[i + 2] = old[i];
        adaptive_vtable[7] = (uintptr_t)&s25_adaptive_set;
        adaptive_vtable[10] = (uintptr_t)&s25_adaptive_peer;
    }
    __atomic_clear(&vlock, __ATOMIC_RELEASE);
    *(uintptr_t **)obj = adaptive_vtable + 2;
    fix(obj);
    return obj;
}
extern "C" bool s25_supported_ss(int index) {
    return index == s25::ll::kPrivateIndex || index == s25::kPrivateAdaptive
               ? true
               : tramp_supported_ss(index);
}
extern "C" int s25_source_index(const uint8_t *c) {
    return s25::ll::proxy(c)     ? s25::ll::kPrivateIndex
           : s25::proxy_codec(c) ? s25::kPrivateAdaptive
                                 : tramp_source_index(c);
}
extern "C" uint64_t s25_provider_index(const uint8_t *c) {
    return s25::ll::proxy(c) || s25::proxy_codec(c) ? 0 : tramp_provider_index(c);
}
extern "C" int s25_vendor_source_index(const uint8_t *c) {
    return s25::ll::proxy(c)     ? s25::ll::kPrivateIndex
           : s25::proxy_codec(c) ? s25::kPrivateAdaptive
                                 : tramp_vendor_source_index(c);
}
extern "C" bool s25_init_codec(int index, void *cfg) {
    if (index == s25::ll::kPrivateIndex) {
        uint8_t local[20] = {13, 0, 255, 0xd7, 0, 0, 0, 0x24, 0, 0x32, 0xa5, 0x4c, 255, 0};
        memcpy(cfg, local, 20);
        return true;
    }
    bool result = tramp_init_codec(index == s25::kPrivateAdaptive ? 3 : index, cfg);
    if (result && index == s25::kPrivateAdaptive)
        s25::mark_proxy((uint8_t *)cfg);
    return result;
}
#endif
#ifndef S25_HAL_ONLY
extern "C" bool tramp_type_eq(const uint8_t *, const uint8_t *);
extern "C" bool tramp_equal(const uint8_t *, const uint8_t *);
extern "C" bool tramp_vendor_type_eq(const uint8_t *, const uint8_t *);
extern "C" bool tramp_hd_type_eq(const uint8_t *, const uint8_t *);
extern "C" bool tramp_hd_equal(const uint8_t *, const uint8_t *);
static bool private_equal(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 14; i++)
        if (a[i] != b[i])
            return false;
    return true;
}
#define TYPE_WRAPPER(name)                                                                         \
    extern "C" bool s25_##name(const uint8_t *a, const uint8_t *b) {                               \
        bool aa = s25::proxy_codec(a), bb = s25::proxy_codec(b);                                   \
        bool al = s25::ll::proxy(a), bl = s25::ll::proxy(b);                                       \
        return aa || bb || al || bl ? (aa && bb) || (al && bl) : tramp_##name(a, b);               \
    }
#define VALUE_WRAPPER(name)                                                                        \
    extern "C" bool s25_##name(const uint8_t *a, const uint8_t *b) {                               \
        bool aa = s25::proxy_codec(a), bb = s25::proxy_codec(b);                                   \
        bool al = s25::ll::proxy(a), bl = s25::ll::proxy(b);                                       \
        return aa || bb || al || bl ? ((aa && bb) || (al && bl)) && private_equal(a, b)            \
                                    : tramp_##name(a, b);                                          \
    }
TYPE_WRAPPER(type_eq)
TYPE_WRAPPER(vendor_type_eq)
TYPE_WRAPPER(hd_type_eq)
VALUE_WRAPPER(equal)
VALUE_WRAPPER(hd_equal)
#endif
