// SPDX-License-Identifier: GPL-2.0-only
#include <jni.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "../src/layout.h"
#include "../src/aptx_ll.h"
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
extern "C" uint8_t tramp_user(void *, const uint8_t *, const uint8_t *, const uint8_t *, bool,
                              uint8_t *, bool *, bool *, bool *) {
    return 0;
}
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
    puts("JNI LL item: unique public13/0x02000aff, both registry branches, "
         "classic/Adaptive/Lossless preserved; LL request maps to its own private9, never ordinary "
         "aptX2");
}
