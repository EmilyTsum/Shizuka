// SPDX-License-Identifier: GPL-2.0-only
#include <jni.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "../src/layout.h"
#include "../src/aptx_ll.h"
#include "../src/settings.h"
#include "../src/coexist.h"
struct Type {
    int index;
    long long id;
    const char *name;
};
struct Array {
    jsize length;
    jobject items[40]{};
};
static Type objects[128];
static unsigned object_count;
static Array arrays[16], original;
static unsigned array_count;
static std::vector<uint8_t> memory(0xeb0000);
alignas(16) static uint8_t request[64];
extern "C" uintptr_t s25_test_ui_base() { return (uintptr_t)memory.data(); }
extern "C" void s25_anchor() {}
extern "C" jobject tramp_new_type(void *, int) { return nullptr; }
extern "C" jobjectArray tramp_supported(JNIEnv *, jobject) { return (jobjectArray)&original; }
extern "C" s25::Vector tramp_prepare(JNIEnv *, jobjectArray) {
    return {request, request + 64, request + 64};
}
static uint8_t local_status;
extern "C" uint8_t tramp_user(void *, const uint8_t *, const uint8_t *, const uint8_t *, bool,
                              uint8_t *, bool *, bool *, bool *) {
    return local_status;
}
extern "C" uint8_t s25_user(void *, const uint8_t *, const uint8_t *, const uint8_t *, bool,
                            uint8_t *, bool *, bool *, bool *);
extern "C" void tramp_callback(void *, uint8_t *, s25::Vector *, s25::Vector *, unsigned,
                               unsigned) {}
static jstring string(JNIEnv *, const char *s) { return (jstring)s; }
static jobject object(JNIEnv *, jclass, jmethodID, const jvalue *a) {
    auto &o = objects[object_count++];
    o = {a[0].i, a[1].j, (const char *)a[2].l};
    return (jobject)&o;
}
static void drop(JNIEnv *, jobject) {}
static jsize length(JNIEnv *, jarray a) { return ((Array *)a)->length; }
static jobjectArray array(JNIEnv *, jsize n, jclass, jobject) {
    auto &a = arrays[array_count++];
    a.length = n;
    return (jobjectArray)&a;
}
static jobject get(JNIEnv *, jobjectArray a, jsize i) {
    assert(i >= 0 && i < ((Array *)a)->length);
    return ((Array *)a)->items[i];
}
static void set(JNIEnv *, jobjectArray a, jsize i, jobject value) {
    assert(i >= 0 && i < ((Array *)a)->length);
    ((Array *)a)->items[i] = value;
}
extern "C" jobject s25_new_type(void *, int);
extern "C" jobjectArray s25_supported(JNIEnv *, jobject);
extern "C" s25::Vector s25_prepare(JNIEnv *, jobjectArray);
int main() {
    JNINativeInterface_ api{};
    api.NewStringUTF = string;
    api.NewObjectA = object;
    api.DeleteLocalRef = drop;
    api.GetArrayLength = length;
    api.NewObjectArray = array;
    api.GetObjectArrayElement = get;
    api.SetObjectArrayElement = set;
    JNIEnv env{&api};
    JNIEnv *ep = &env;
    auto *meta = (uintptr_t *)(memory.data() + 0xeae890);
    meta[0] = 1;
    meta[1] = 2;
    for (unsigned variant : {0u, 1u, 2u}) {
        bool has_adaptive = variant > 0, has_ll = variant == 2;
        original.length = has_ll ? 4 : has_adaptive ? 3 : 2;
        Type initial[4] = {{0, 0, "SBC"},
                           {2, 0x01004fff, "aptX"},
                           {5, 0xad00d7ff, "aptX Adaptive"},
                           {9, 0x02000aff, "aptX LL"}};
        alignas(16) uint8_t native[4 * 128]{};
        for (int i = 0; i < original.length; i++) {
            original.items[i] = (jobject)&initial[i];
            *(uint32_t *)(native + i * 128 + 0x38) = initial[i].index;
        }
        *(s25::Vector *)(memory.data() + 0xeae6c0) = {native, native + original.length * 128,
                                                      native + sizeof native};
        auto *values = (Array *)s25_supported(&env, nullptr);
        assert(values->length == 5);
        unsigned ll = 0, lossless = 0, adaptive = 0, aptx = 0;
        for (int i = 0; i < values->length; i++) {
            auto &t = *(Type *)values->items[i];
            if (t.index == 13) {
                ll++;
                assert(t.id == s25::ll::kPublicId && !strcmp(t.name, "aptX LL"));
            }
            if (t.index == 12)
                lossless++;
            if (t.index == 7)
                adaptive++;
            if (t.index == 2) {
                aptx++;
                assert(t.id != s25::ll::kPublicId);
            }
        }
        assert(ll == 1 && lossless == 1 && adaptive == 1 && aptx == 1);
    }
    auto *t = (Type *)s25_new_type(&ep, 13);
    assert(t->index == 13 && t->id == s25::ll::kPublicId);
    memset(request, 0, 64);
    *(uint32_t *)request = 13;
    *(uint32_t *)(request + 8) = 2;
    *(uint32_t *)(request + 12) = 1;
    auto v = s25_prepare(&env, nullptr);
    assert(*(uint32_t *)v.begin == 9); // Independent LL object, never ordinary aptX2.
    *(uint32_t *)request = 2;
    v = s25_prepare(&env, nullptr);
    assert(*(uint32_t *)v.begin == 2);
    // A completed local configuration can fail later in HAL/AVDTP. Retrying the same
    // user selection must produce a distinct private config and fresh restart flags.
    auto prepare_adaptive = [&](unsigned priority, unsigned rate, unsigned bits = 2) {
        memset(request, 0, sizeof request);
        *(uint32_t *)request = 7;
        *(uint32_t *)(request + 4) = priority;
        *(uint32_t *)(request + 8) = rate;
        *(uint32_t *)(request + 12) = bits;
        *(uint32_t *)(request + 16) = 2;
        *(uint64_t *)(request + 24) = 4096;
        return s25_prepare(&env, nullptr);
    };
    auto apply_local = [&](uint8_t status, bool restart) {
        local_status = status;
        bool in = false, out = false, updated = false;
        uint8_t audio[64]{}, peer[20]{}, wire[20]{};
        assert(s25_user(nullptr, request, audio, peer, false, wire, &in, &out, &updated) ==
               status);
        assert(in == restart && out == restart && updated == restart);
    };
    prepare_adaptive(1000000, 2);
    assert(*(uint32_t *)request == 5 && s25::current_rate() == 48000 &&
           s25::current_bits() == 24 && s25::current_mode() == 4096);
    auto clock = *(uint32_t *)(request + 8);
    auto descriptor = *(uint64_t *)(request + 48);
    apply_local(0, true); // Local flags consumed before a simulated downstream failure.
    assert(!s25::consume_settings_change());
    prepare_adaptive(1000000, 2); // Same logical 48/HQ retry.
    assert(*(uint32_t *)(request + 8) != clock);
    assert(*(uint64_t *)(request + 48) == descriptor); // Actual requested rate unchanged.
    apply_local(0, true);
    clock = *(uint32_t *)(request + 8);
    prepare_adaptive(4001, 2); // Startup/default vectors must not provoke another restart.
    assert(*(uint32_t *)(request + 8) == clock);
    apply_local(0, false);

    // Local rejection must not consume the retry; invalid PCM depth cannot flip state.
    prepare_adaptive(1000000, 1);
    assert(s25::current_rate() == 44100);
    apply_local(0x29, false);
    apply_local(0, true);
    clock = s25::proxy_rate();
    prepare_adaptive(1000000, 8, 1);
    assert(*(uint32_t *)request == 1000000 && s25::current_rate() == 44100 &&
           s25::proxy_rate() == int(clock) && !s25::consume_settings_change());

    auto prepare_lossless = [&]() {
        memset(request, 0, sizeof request);
        *(uint32_t *)request = s25::kPublicLossless;
        *(uint32_t *)(request + 4) = 1000000;
        *(uint32_t *)(request + 8) = 1;
        *(uint32_t *)(request + 12) = 1; // Public CD16 alias, DSP must remain PCM24.
        *(uint64_t *)(request + 24) = s25::kModeLossless;
        s25_prepare(&env, nullptr);
        assert(*(uint32_t *)request == s25::kPrivateAdaptive &&
               *(uint32_t *)(request + 12) == 2 && s25::current_bits() == 24 &&
               s25::current_rate() == 44100 && s25::current_mode() == s25::kModeLossless);
    };
    prepare_lossless();
    clock = *(uint32_t *)(request + 8);
    descriptor = *(uint64_t *)(request + 48);
    apply_local(0, true);
    prepare_lossless();
    assert(*(uint32_t *)(request + 8) != clock &&
           *(uint64_t *)(request + 48) == descriptor);
    apply_local(0, true);

    puts("JNI explicit Adaptive/Lossless retry: same desired rate gets a distinct proxy and renewed "
         "restart flags after downstream failure; defaults stay idempotent; local rejection "
         "and invalid depth preserve state");
    puts("JNI LL item: unique public13/0x02000aff, both registry branches, "
         "classic/Adaptive/Lossless preserved; LL request maps to its own private9, never ordinary "
         "aptX2");
}
