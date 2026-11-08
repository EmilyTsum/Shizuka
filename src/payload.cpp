// SPDX-License-Identifier: GPL-2.0-only
#include "protocol.h"
#include "layout.h"
#include "settings.h"
#include "coexist.h"
#include "aptx_ll_native.h"
extern "C" __attribute__((visibility("hidden"))) void s25_anchor();
extern "C" uint8_t tramp_prs_cfg(void *, uint8_t *, uint16_t, uint8_t *, uint8_t);
extern "C" void tramp_bld_cfg(uint8_t **, void *);
extern "C" __attribute__((visibility("hidden"))) bool tramp_to_hal(uint8_t *, void *);
extern "C" bool tramp_enabled(const uint8_t *);
extern "C" __attribute__((visibility("hidden"))) bool tramp_specific(void *, uint8_t *);
extern "C" bool tramp_validate(const uint8_t *, const uint8_t *);
struct VscComplete {
    uint16_t opcode;
    uint16_t length;
    uint8_t *bytes;
};
using VscCallback = void (*)(VscComplete *);
extern "C" void tramp_vendor(uint16_t, uint8_t, uint8_t *, VscCallback);
extern "C" void *memcpy(void *d, const void *s, size_t n) {
    auto *a = (uint8_t *)d;
    auto *b = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++)
        a[i] = b[i];
    return d;
}
extern "C" void *memset(void *d, int c, size_t n) {
    auto *a = (uint8_t *)d;
    for (size_t i = 0; i < n; i++)
        a[i] = (uint8_t)c;
    return d;
}
static uintptr_t base() { return reinterpret_cast<uintptr_t>(&s25_anchor) - 0x2000000; }
static s25::State state;
static uint8_t state_lock;
struct Lock {
    Lock() {
        while (__atomic_test_and_set(&state_lock, __ATOMIC_ACQUIRE)) {
        }
    }
    ~Lock() { __atomic_clear(&state_lock, __ATOMIC_RELEASE); }
};
extern "C" __attribute__((visibility("default"))) const s25::State *s25_debug_state() {
    return &state;
}
#ifndef S25_HAL_ONLY
static VscCallback start_callback, stop_callback;
// Ownership begins when START is submitted, not when its completion arrives.
// The stack may cancel a pending start; keep that STOP in the same QTI protocol.
static bool qti_owned, qti_start_pending, qti_stop_pending;
static bool qti_stop_queued, qti_prior_owned, native_start_pending;
static uint16_t start_mode = 4096;
static void qti_response(VscComplete *p, VscCallback cb, uint8_t sub) {
    if (!cb)
        return;
    uint8_t bytes[2] = {uint8_t(p && p->length && p->bytes ? p->bytes[0] : 0x1f), sub};
    VscComplete mapped{0xfd5d, 2, bytes};
    cb(&mapped);
}
static bool qti_success(VscComplete *p) {
    return p && p->length && p->bytes && p->bytes[0] == 0;
}
static void qti_stop_response(VscComplete *p) {
    // A failed stop does not establish that the controller released its stream.
    if (qti_success(p))
        qti_owned = false;
    qti_stop_pending = false;
    auto cb = stop_callback;
    stop_callback = nullptr;
    qti_response(p, cb, 2);
}
static void qti_send_stop() {
    uint8_t command[2] = {5, 0};
    tramp_vendor(0xfc0a, 2, command, qti_stop_response);
}
static void qti_start_response(VscComplete *p) {
    bool stopping = qti_stop_queued;
    qti_start_pending = false;
    qti_stop_queued = false;
    // A failed fresh START cannot create ownership, even when cancellation was queued.
    // A failed replacement START must not forget an already running QTI stream.
    qti_owned = qti_success(p) || qti_prior_owned;
    if (qti_success(p) && !stopping) {
        // VS_QHCI_ENCODER_MODE_CHANGE, one TLV, encoder-mode id5, uint16 HQ/LL.
        uint8_t mode[6] = {0x13, 1, 5, 2, uint8_t(start_mode), uint8_t(start_mode >> 8)};
        tramp_vendor(0xfc0a, 6, mode, nullptr);
    }
    auto cb = start_callback;
    start_callback = nullptr;
    qti_response(p, cb, 1);
    // Deliver START before STOP, and do not issue a late mode update after cancellation.
    if (stopping)
        qti_send_stop();
}
// A native codec can replace a QTI stream after a failed STOP or link recovery.
// Transfer protocol ownership only when that native START actually succeeds.
static void native_start_response(VscComplete *p) {
    bool stopping = qti_stop_queued;
    native_start_pending = false;
    qti_stop_queued = false;
    if (qti_success(p))
        qti_owned = false;
    auto cb = start_callback;
    start_callback = nullptr;
    if (cb)
        cb(p);
    if (stopping) {
        if (qti_owned)
            qti_send_stop();
        else {
            uint8_t command = 2;
            tramp_vendor(0xfd5d, 1, &command, qti_stop_response);
        }
    }
}
static void native_start(uint8_t n, uint8_t *p, VscCallback cb) {
    if (!qti_owned) {
        tramp_vendor(0xfd5d, n, p, cb);
        return;
    }
    start_callback = cb;
    native_start_pending = true;
    tramp_vendor(0xfd5d, n, p, native_start_response);
}
static void qti_busy(VscCallback cb, uint8_t sub) {
    uint8_t status = 0x0c; // HCI Command Disallowed: another stream transaction owns the slot.
    VscComplete event{0xfc0a, 1, &status};
    qti_response(&event, cb, sub);
}
extern "C" void s25_vendor(uint16_t opcode, uint8_t n, uint8_t *p, VscCallback cb) {
    if (opcode == 0xfd5d && p && n == 57 && p[0] == 1) {
        if (qti_start_pending || native_start_pending || qti_stop_pending) {
            qti_busy(cb, 1);
            return;
        }
        uint8_t ll_command[57];
        if (s25::ll::offload_start(p, n, ll_command)) {
            native_start(n, ll_command, cb);
            return;
        }
        uint8_t command[66];
        size_t count;
        {
            Lock guard;
            count = s25::qti_start(p, n, &state, command, sizeof command);
            start_mode = state.encoder_mode == 8192 ? 8192 : 4096;
        }
        if (count) {
            start_callback = cb;
            qti_prior_owned = qti_owned;
            qti_owned = qti_start_pending = true;
            tramp_vendor(0xfc0a, (uint8_t)count, command, qti_start_response);
            return;
        }
        native_start(n, p, cb);
        return;
    }
    if (opcode == 0xfd5d && p && n && p[0] == 2 &&
        (qti_owned || qti_start_pending || native_start_pending || qti_stop_pending)) {
        if (qti_stop_pending) {
            qti_busy(cb, 2);
            return;
        }
        stop_callback = cb;
        qti_stop_pending = true;
        if (qti_start_pending || native_start_pending)
            qti_stop_queued = true;
        else
            qti_send_stop();
        return;
    }
    tramp_vendor(opcode, n, p, cb);
}
extern "C" uint8_t s25_prs_cfg(void *cfg, uint8_t *p, uint16_t n, uint8_t *elem, uint8_t sig) {
    if (n > 512) {
        if (elem)
            *elem = 7;
        state.selected_seen = 0;
        return 0x29;
    }
    uint8_t translated[512], local[512];
    size_t translated_size;
    if (!s25::ll::translate(p, n, translated, sizeof translated, &translated_size, false, sig)) {
        if (elem)
            *elem = 7;
        return 0x29;
    }
    s25::Result r;
    {
        Lock guard;
        r = s25::incoming(translated, translated_size, local, sizeof local, sig, &state);
    }
    if (r.error != s25::Error::Ok) {
        if (elem)
            *elem = 7;
        state.selected_seen = 0;
        return 0x29;
    }
    return tramp_prs_cfg(cfg, local, (uint16_t)r.size, elem, sig);
}
extern "C" void s25_bld_cfg(uint8_t **p, void *cfg) {
    uint8_t internal[256], output[256];
    uint8_t *end = internal;
    tramp_bld_cfg(&end, cfg);
    size_t n = (size_t)(end - internal);
    // AVDT_CMD_BUF_SIZE must be established by the profile before this hook is deployable.
    uint8_t sig = 12;
    auto *c = (uint8_t *)cfg;
    if (s25::proxy_codec(c) && (c[9] == 0x12 || c[9] == 0x22))
        sig = 3;
    s25::Result r;
    {
        Lock guard;
        r = s25::outgoing(internal, n, output, sizeof output, sig, &state);
    }
    if (r.error != s25::Error::Ok) {
        state.selected_seen = 0;
        return;
    }
    uint8_t ll_output[256];
    size_t ll_size;
    if (!s25::ll::translate(output, r.size, ll_output, sizeof ll_output, &ll_size, true, sig))
        return;
    memcpy(*p, ll_output, ll_size);
    *p += ll_size;
}
extern "C" bool s25_to_hal(uint8_t *out, void *codec) {
    if (*(uint32_t *)((uint8_t *)codec + 0x30) == s25::ll::kPrivateIndex)
        return s25::ll::hal_view(codec, out, tramp_to_hal);
    s25::ll::deactivate();
    if (*(uint32_t *)((uint8_t *)codec + 0x30) != s25::kPrivateAdaptive)
        return tramp_to_hal(out, codec);
    // Reconstruct a real HD ABI view solely for the stock converter; never mutate the live object.
    alignas(16) uint8_t view[0x1e0]{};
    auto ctor = (void (*)(void *, int))(base() + 0x9771c0);
    ctor(view, *(int *)((uint8_t *)codec + 0x58));
    for (unsigned off = 0x60; off <= 0x160; off += 0x40) {
        memcpy(view + off, (uint8_t *)codec + off, 64);
        *(uint32_t *)(view + off) = 3;
    }
    for (unsigned off = 0x1a0; off <= 0x1c8; off += 20) {
        memcpy(view + off, (uint8_t *)codec + off, 20);
        if (s25::proxy_codec(view + off))
            memset(view + off + 10, 0, 4);
    }
    bool ok = tramp_to_hal(out, view);
    auto destroy = (void (*)(void *))(*(uintptr_t **)view)[0];
    destroy(view);
    if (!ok || *(uint32_t *)out != 4)
        return false;
    uint8_t selected[43];
    if (!s25::proxy_selection((uint8_t *)codec + 0x1a0, selected))
        return false;
    {
        Lock guard;
        memcpy(state.selected, selected, 43);
        state.selected_seen = 1;
        state.encoder_mode = s25::hal_mode(s25::flags_mode(((uint8_t *)codec)[0x1ad]));
    }
    // The original converter left an aptX POD variant (tag 3); no owning vector is discarded.
    if (*(uint32_t *)(out + 0x58) != 3)
        return false;
    auto new_bytes = reinterpret_cast<void *(*)(size_t)>(
        *reinterpret_cast<uintptr_t *>(base() + S25_MALLOC_GOT));
    auto *stream = (uint8_t *)new_bytes(11);
    if (!stream)
        return false;
    memcpy(stream, selected + 18, 11);
    memset(out + 0x10, 0, 0x48);
    auto *a = (s25::AdaptiveConfig *)(out + 0x18);
    a->sample_rate = (selected[9] & 0xf8) == 0xa0   ? 96000
                     : (selected[9] & 0xf8) == 0x40 ? 44100
                                                    : 48000;
    a->channel_mode = 0;
    a->bits = s25::flags_bits(((uint8_t *)codec)[0x1ad]);
    a->mode = s25::hal_mode(s25::flags_mode(((uint8_t *)codec)[0x1ad]));
    for (unsigned i = 0; i < 6; i++) {
        a->buffering[i] = i % 2 ? 50 : 20;
        a->ttp[i] = (int8_t)(selected[11 + i] - 128);
    }
    // Lossless HQ bounds request, not measured buffering. R2.1 packing doubles
    // the max field, but the inspected DSP INIT ignores maxHQ; min60 is below
    // its observed TTP candidate and does not establish added playout delay.
    if (s25::flags_mode(((uint8_t *)codec)[0x1ad]) == s25::kModeLossless) {
        a->buffering[2] = 60;
        a->buffering[3] = 100;
    }
    a->fade_ms = 255;
    a->stream = {stream, stream + 11, stream + 11};
    *(uint32_t *)(out + 0x58) = 4;
    *(uint32_t *)out = 9;
    return s25::adaptive_valid(out);
}
extern "C" bool s25_adjust_offload(uint8_t *config) {
    if (*(uint32_t *)config != 8)
        return false;
    uint8_t rate;
    {
        Lock guard;
        if (!state.selected_seen || !s25::selection_valid(state.selected, 43))
            return false;
        rate = state.selected[9] & 0xf8;
    }
    *(uint32_t *)config = 0x40;
    *(uint32_t *)(config + 8) = rate == 0xa0 ? 8 : rate == 0x40 ? 1 : 2;
    config[12] = s25::current_bits() == 24 ? 2 : 1;
    const uint8_t id[6] = {0xd7, 0, 0, 0, 0xad, 0};
    memcpy(config + 0x1a, id, 6);
    return true;
}
extern "C" bool s25_specific(void *codec, uint8_t *config) {
    if (*(uint32_t *)((uint8_t *)codec + 0x30) == s25::ll::kPrivateIndex)
        return s25::ll::specific_view(codec, config, tramp_specific);
    s25::ll::deactivate();
    bool ok = tramp_specific(codec, config);
    if (ok && *(uint32_t *)((uint8_t *)codec + 0x30) == s25::kPrivateAdaptive) {
        uint8_t selected[43];
        if (!s25::proxy_selection((uint8_t *)codec + 0x1a0, selected))
            return false;
        {
            Lock guard;
            memcpy(state.selected, selected, 43);
            state.selected_seen = 1;
            state.encoder_mode = s25::hal_mode(s25::flags_mode(((uint8_t *)codec)[0x1ad]));
        }
        *(uint32_t *)config = 8;
        if (!s25_adjust_offload(config))
            return false;
        config[12] = s25::flags_bits(((uint8_t *)codec)[0x1ad]) == 16 ? 1 : 2;
    }
    return ok;
}
extern "C" bool s25_enabled(const uint8_t *cfg) {
    // Caller owns a mutable local configuration; apply LL's budget before it is sent to HAL.
    s25::ll::limit_hal(const_cast<uint8_t *>(cfg));
    if (*(const uint32_t *)cfg == 9)
        return state.selected_seen && s25::adaptive_valid(cfg);
    return tramp_enabled(cfg);
}
#else
extern "C" bool s25_validate(const uint8_t *session, const uint8_t *cfg) {
    if (*(const uint32_t *)cfg == 9)
        return (*session == 2 || *session == 11) && s25::adaptive_valid(cfg);
    return tramp_validate(session, cfg);
}
#endif
