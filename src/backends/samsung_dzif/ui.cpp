// SPDX-License-Identifier: GPL-2.0-only
#ifndef S25_HAL_ONLY
#include <jni.h>
#include "layout.h"
#include "settings.h"
#include "coexist.h"
#include "aptx_ll_native.h"
extern "C" __attribute__((visibility("hidden"))) void s25_anchor();
extern "C" jobject tramp_new_type(void *, int);
extern "C" jobjectArray tramp_supported(JNIEnv *, jobject);
extern "C" s25::Vector tramp_prepare(JNIEnv *, jobjectArray);
extern "C" uint8_t tramp_user(void *, const uint8_t *, const uint8_t *, const uint8_t *, bool,
                              uint8_t *, bool *, bool *, bool *);
extern "C" uint8_t s25_user(void *codec, const uint8_t *user, const uint8_t *audio,
                            const uint8_t *peer, bool capability, uint8_t *out, bool *input,
                            bool *output, bool *updated) {
    bool ll_switch = *(uint32_t *)user == s25::ll::kPrivateIndex && !s25::ll::proxy(peer);
    uint8_t ll_peer[20];
    if (*(uint32_t *)user == s25::ll::kPrivateIndex) {
        if (!s25::ll::user_peer(codec, peer, ll_peer))
            return 0x29;
        peer = ll_peer;
    }
    uint8_t status = tramp_user(codec, user, audio, peer, capability, out, input, output, updated);
    if (status == 0 && ll_switch) {
        if (output)
            *output = true;
        if (updated)
            *updated = true;
    }
    if (status == 0 && *(uint32_t *)user == s25::kPrivateAdaptive &&
        s25::consume_settings_change()) {
        if (input)
            *input = true;
        if (output)
            *output = true;
        if (updated)
            *updated = true;
    }
    return status;
}
extern "C" void tramp_callback(void *, uint8_t *, s25::Vector *, s25::Vector *, unsigned, unsigned);
extern "C" void *memcpy(void *, const void *, size_t);
#ifdef S25_UI_TEST_BASE
extern "C" uintptr_t s25_test_ui_base();
static uintptr_t base() { return s25_test_ui_base(); }
#else
static uintptr_t base() { return (uintptr_t)&s25_anchor - 0x2000000; }
#endif
static constexpr jint kAdaptiveIndex = 7;
static constexpr jlong kAdaptiveId = 0xad00d7ffLL;
static jobject named_type(JNIEnv *env, int index) {
    auto *meta = (uintptr_t *)(base() + 0xeae890);
    if (!env || !meta[0] || !meta[1])
        return nullptr;
    jstring name = env->NewStringUTF(
        (index == s25::ll::kPublicIndex || index == s25::ll::kPrivateIndex) ? "aptX LL"
        : index == s25::kPublicLossless                                     ? "aptX Lossless"
        : index == s25::kPrivateAdaptive                                    ? "aptX Adaptive"
        : index == 3                                                        ? "aptX HD"
                                                                            : "aptX");
    if (!name)
        return nullptr;
    jvalue args[3]{};
    args[0].i = index == s25::ll::kPrivateIndex  ? s25::ll::kPublicIndex
                : index == s25::kPrivateAdaptive ? kAdaptiveIndex
                                                 : index;
    args[1].j = (index == s25::ll::kPublicIndex || index == s25::ll::kPrivateIndex)
                    ? s25::ll::kPublicId
                : index == s25::kPublicLossless  ? s25::kPublicLosslessId
                : index == s25::kPrivateAdaptive ? kAdaptiveId
                : index == 3                     ? 0x2400d7ffLL
                                                 : 0x01004fffLL;
    args[2].l = name;
    jobject result = env->NewObjectA((jclass)meta[0], (jmethodID)meta[1], args);
    env->DeleteLocalRef(name);
    return result;
}
extern "C" jobject s25_new_type(void *callback_env, int index) {
    if (index == s25::ll::kPrivateIndex || index == s25::ll::kPublicIndex ||
        index == s25::kPublicLossless || index == s25::kPrivateAdaptive || index == 2 || index == 3)
        return named_type(*(JNIEnv **)callback_env, index);
    return tramp_new_type(callback_env, index);
}
extern "C" jobjectArray s25_supported(JNIEnv *env, jobject object) {
    jobjectArray result = tramp_supported(env, object);
    if (!result)
        return result;
    auto *v = (s25::Vector *)(base() + 0xeae6c0);
    uintptr_t count = ((uintptr_t)v->end - (uintptr_t)v->begin) / 128;
    if (!v->begin || count > 32 || (uintptr_t)v->end < (uintptr_t)v->begin)
        return result;
    jsize length = env->GetArrayLength(result);
    bool found = false, found_ll = false;
    for (uintptr_t i = 0; i < count; i++)
        if (*(uint32_t *)(v->begin + i * 128 + 0x38) == s25::ll::kPrivateIndex)
            found_ll = true;
    for (uintptr_t i = 0; i < count; i++)
        if (*(uint32_t *)(v->begin + i * 128 + 0x38) == s25::kPrivateAdaptive)
            found = true;
    for (uintptr_t i = 0; i < count && i < (uintptr_t)length; i++)
        if (*(uint32_t *)(v->begin + i * 128 + 0x38) == s25::kPrivateAdaptive ||
            *(uint32_t *)(v->begin + i * 128 + 0x38) == s25::ll::kPrivateIndex ||
            *(uint32_t *)(v->begin + i * 128 + 0x38) == 2 ||
            *(uint32_t *)(v->begin + i * 128 + 0x38) == 3) {
            jobject type = named_type(env, *(uint32_t *)(v->begin + i * 128 + 0x38));
            if (!type)
                return result;
            env->SetObjectArrayElement(result, (jsize)i, type);
            env->DeleteLocalRef(type);
        }
    if (!found) {
        auto *meta = (uintptr_t *)(base() + 0xeae890);
        jobjectArray extended = env->NewObjectArray(length + 2, (jclass)meta[0], nullptr);
        if (!extended)
            return result;
        for (jsize i = 0; i < length; i++) {
            jobject item = env->GetObjectArrayElement(result, i);
            env->SetObjectArrayElement(extended, i, item);
            env->DeleteLocalRef(item);
        }
        jobject type = named_type(env, s25::kPrivateAdaptive);
        if (!type)
            return result;
        env->SetObjectArrayElement(extended, length, type);
        env->DeleteLocalRef(type);
        type = named_type(env, s25::kPublicLossless);
        if (!type)
            return result;
        env->SetObjectArrayElement(extended, length + 1, type);
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(result);
        result = extended;
    } else {
        auto *meta = (uintptr_t *)(base() + 0xeae890);
        jobjectArray extended = env->NewObjectArray(length + 1, (jclass)meta[0], nullptr);
        if (!extended)
            return result;
        for (jsize i = 0; i < length; i++) {
            jobject item = env->GetObjectArrayElement(result, i);
            env->SetObjectArrayElement(extended, i, item);
            env->DeleteLocalRef(item);
        }
        jobject type = named_type(env, s25::kPublicLossless);
        if (!type)
            return result;
        env->SetObjectArrayElement(extended, length, type);
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(result);
        result = extended;
    }
    if (found_ll)
        return result;
    auto *llmeta = (uintptr_t *)(base() + 0xeae890);
    jsize llcount = env->GetArrayLength(result);
    jobjectArray with_ll = env->NewObjectArray(llcount + 1, (jclass)llmeta[0], nullptr);
    if (!with_ll)
        return result;
    for (jsize i = 0; i < llcount; i++) {
        jobject item = env->GetObjectArrayElement(result, i);
        env->SetObjectArrayElement(with_ll, i, item);
        env->DeleteLocalRef(item);
    }
    jobject lltype = named_type(env, s25::ll::kPublicIndex);
    if (!lltype)
        return result;
    env->SetObjectArrayElement(with_ll, llcount, lltype);
    env->DeleteLocalRef(lltype);
    env->DeleteLocalRef(result);
    return with_ll;
}
extern "C" s25::Vector s25_prepare(JNIEnv *env, jobjectArray configs) {
    s25::Vector v = tramp_prepare(env, configs);
    uintptr_t n = (uintptr_t)v.end - (uintptr_t)v.begin;
    if (!v.begin || n > 32 * 64 || n % 64)
        return v;
    for (uint8_t *p = v.begin; p < v.end; p += 64)
        if (*(uint32_t *)p == s25::ll::kPublicIndex) {
            unsigned rate = *(uint32_t *)(p + 8), bits = *(uint32_t *)(p + 12),
                     channels = *(uint32_t *)(p + 16);
            if ((rate && rate != 1 && rate != 2) || (bits && bits != 1) ||
                (channels && channels != 2)) {
                *(uint32_t *)p = 1000000;
                continue;
            }
            *(uint32_t *)p = s25::ll::kPrivateIndex;
            if (!rate)
                *(uint32_t *)(p + 8) = 2;
            if (!bits)
                *(uint32_t *)(p + 12) = 1;
            for (unsigned off = 24; off < 56; off += 8)
                *(uint64_t *)(p + off) = 0;
        }
    for (uint8_t *p = v.begin; p < v.end; p += 64)
        if (*(uint32_t *)p == kAdaptiveIndex || *(uint32_t *)p == s25::kPublicLossless) {
            bool lossless = *(uint32_t *)p == s25::kPublicLossless;
            unsigned r = *(uint32_t *)(p + 8), b = *(uint32_t *)(p + 12);
            uint64_t mode = *(uint64_t *)(p + 24);
            int selected_mode = lossless ? s25::kModeLossless
                                : mode
                                    ? (mode == 4096 || mode == 12288 ? 4096
                                       : mode == 8192                ? 8192
                                                                     : 0)
                                    : (s25::current_mode() == 12288 ? 4096 : s25::current_mode());
            // Selecting Adaptive must leave the separate Lossless entry, including rate=NONE.
            if (!lossless && mode == 0)
                selected_mode = s25::current_mode() == 12288 ? 4096 : s25::current_mode();
            int hz = selected_mode == 12288 ? 44100
                     : r                    ? (r == 8                    ? 96000
                                               : r == 2                  ? 48000
                                               : r == 1 && s25::kR2 == 3 ? 44100
                                                                         : 0)
                                            : s25::current_rate();
            // Lossless's public 16-bit value describes CD precision, not the DSP PCM container.
            // Normalize the UI alias here; all native/HAL/DSP input remains PCM24.
            int bits = lossless ? 24 : b ? (b == 2 ? 24 : b == 1 ? 16 : 0) : 24;
            // Samsung consumes local restart flags before asynchronous HAL/AVDTP application.
            // A retry must traverse its deferred device reconfigure path even when our desired
            // values already match. Only an explicit user selection gets a fresh HD clock;
            // startup/capability vectors retain idempotent settings.
            bool retryable = *(uint32_t *)(p + 4) == 1000000 && (r || mode || lossless);
            if (!s25::request_settings(hz, bits, selected_mode, retryable)) {
                *(uint32_t *)p = 1000000;
                continue;
            }
            *(uint32_t *)p = s25::kPrivateAdaptive;
            if (r || mode || lossless || s25::current_mode() == 4096)
                *(uint64_t *)(p + 48) = s25::kRequestDescriptor |
                                        ((r && selected_mode != 12288) ? 0x100 : 0) |
                                        (hz == 96000   ? 1
                                         : hz == 44100 ? 2
                                                       : 0) |
                                        (selected_mode == 8192    ? 4
                                         : selected_mode == 12288 ? 8
                                                                  : 0);
            // Toggle the private HD 44.1/48k clock to trigger reconfiguration; wire/HAL carry the
            // actual rate.
            if (*(uint32_t *)(p + 8))
                *(uint32_t *)(p + 8) = s25::proxy_rate();
            if (*(uint32_t *)(p + 12))
                *(uint32_t *)(p + 12) = 2;
        }
    return v;
}
static s25::Vector copy_caps(s25::Vector *src, uint8_t *dst, bool selectable) {
    uintptr_t n = (uintptr_t)src->end - (uintptr_t)src->begin;
    if (!src->begin || n > 32 * 64 || n % 64)
        return *src;
    memcpy(dst, src->begin, n);
    uintptr_t extra = 0;
    for (uintptr_t i = 0; i < n; i += 64)
        if (*(uint32_t *)(dst + i) == s25::ll::kPrivateIndex) {
            *(uint32_t *)(dst + i) = s25::ll::kPublicIndex;
            *(uint64_t *)(dst + i + 48) = 0;
        }
    for (uintptr_t i = 0; i < n; i += 64)
        if (*(uint32_t *)(dst + i) == s25::kPrivateAdaptive) {
            uint64_t d = *(uint64_t *)(dst + i + 48);
            unsigned rates = s25::kR2 == 3 ? 11 : s25::kR2 ? 10 : 2;
            if (selectable)
                rates = s25::descriptor_valid(d) ? s25::proxy_caps((d >> 8) & 255) : 0;
            *(uint32_t *)(dst + i + 8) = rates;
            *(uint32_t *)(dst + i + 12) = 2;
            *(uint64_t *)(dst + i + 48) = 0;
            if (selectable && s25::descriptor_valid(d))
                *(uint64_t *)(dst + i + 40) = s25::proxy_r22((d >> 8) & 255) ? 0x2000 : 0x1000;
            if (s25::kR2 == 3 && !extra &&
                (!selectable ||
                 (s25::descriptor_valid(d) && s25::proxy_r22((d >> 8) & 255) && (rates & 1)))) {
                uint8_t *alias = dst + n;
                memcpy(alias, dst + i, 64);
                *(uint32_t *)alias = s25::kPublicLossless;
                *(uint32_t *)(alias + 8) = 1;
                *(uint32_t *)(alias + 12) = 1;
                *(uint64_t *)(alias + 24) = s25::kModeLossless;
                extra = 64;
            }
        }
    return {dst, dst + n + extra, dst + n + extra};
}
extern "C" void s25_callback(void *addr, uint8_t *cfg, s25::Vector *local, s25::Vector *selectable,
                             unsigned arg4, unsigned arg5) {
    alignas(16) uint8_t current[64], l[33 * 64], s[33 * 64];
    memcpy(current, cfg, 64);
    if (*(uint32_t *)current == s25::ll::kPrivateIndex) {
        *(uint32_t *)current = s25::ll::kPublicIndex;
        *(uint64_t *)(current + 48) = 0;
    }
    if (*(uint32_t *)current == s25::kPrivateAdaptive) {
        uint64_t d = *(uint64_t *)(current + 48);
        if (s25::descriptor_valid(d) && ((d >> 8) & 255) < 64) {
            int rate = s25::flags_rate(d & 15);
            *(uint32_t *)(current + 8) = rate == 96000 ? 8 : rate == 44100 ? 1 : 2;
            *(uint32_t *)(current + 12) = s25::flags_mode(d & 15) == s25::kModeLossless ? 1 : 2;
            if (s25::flags_mode(d & 15) == s25::kModeLossless)
                *(uint32_t *)current = s25::kPublicLossless;
            *(uint64_t *)(current + 24) = s25::flags_mode(d & 15);
            *(uint64_t *)(current + 40) = s25::proxy_r22((d >> 8) & 255) ? 0x2000 : 0x1000;
        }
        *(uint64_t *)(current + 48) = 0;
    }
    s25::Vector lc = copy_caps(local, l, false), sc = copy_caps(selectable, s, true);
    // Disassembly confirms these three incoming value parameters are not freed by the callee.
    tramp_callback(addr, current, &lc, &sc, arg4, arg5);
}
#endif
