// SPDX-License-Identifier: GPL-2.0-only
#include "protocol.h"
#include "settings.h"
#include "coexist.h"
namespace s25 {
static uint32_t settings_word = (kRate == 96000   ? 1u
                                 : kRate == 44100 ? 2u
                                                  : 0u) |
                                (kMode == 8192    ? 4u
                                 : kMode == 12288 ? 8u
                                                  : 0u);
static uint8_t settings_dirty;
int current_rate() {
    uint32_t v = __atomic_load_n(&settings_word, __ATOMIC_ACQUIRE) & 3;
    return v == 1 ? 96000 : v == 2 ? 44100 : 48000;
}
int proxy_rate() { return (__atomic_load_n(&settings_word, __ATOMIC_ACQUIRE) & 16) ? 1 : 2; }
int current_bits() { return 24; }
int current_mode() {
    uint32_t v = __atomic_load_n(&settings_word, __ATOMIC_ACQUIRE);
    return (v & 8) ? 12288 : (v & 4) ? 8192 : 4096;
}
bool request_settings(int rate, int bits, int mode) {
    if (mode == 12288 && kR2 == 3)
        rate = 44100;
    if ((rate != 48000 && rate != 96000 && !(kR2 == 3 && rate == 44100)) || bits != 24 ||
        (mode != 4096 && mode != 8192 && !(kR2 == 3 && mode == 12288)))
        return false;
    uint32_t word = (rate == 96000   ? 1u
                     : rate == 44100 ? 2u
                                     : 0u) |
                    (mode == 8192    ? 4u
                     : mode == 12288 ? 8u
                                     : 0u);
    uint32_t old = __atomic_load_n(&settings_word, __ATOMIC_ACQUIRE);
    while ((old & 15) != word) {
        uint32_t next = word | ((old ^ 16) & 16);
        if (__atomic_compare_exchange_n(&settings_word, &old, next, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&settings_dirty, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    return true;
}
bool consume_settings_change() { return __atomic_exchange_n(&settings_dirty, 0, __ATOMIC_ACQ_REL); }
extern "C" bool s25_request_settings(int rate, int bits, int mode) {
    return request_settings(rate, bits, mode);
}

static void copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
}
static bool vendor(const uint8_t *p, size_t n, uint16_t id) {
    return n >= 8 && p[0] == 0 && p[1] == 255 && p[2] == 0xd7 && p[3] == 0 && p[4] == 0 &&
           p[5] == 0 && p[6] == (id & 255) && p[7] == (id >> 8);
}
static bool marked_payload(const uint8_t *p, size_t n) {
    return n == 13 && p[9] == 0xa5 && p[10] == 0xad && (p[11] < 64 || p[11] == 255) &&
           flags_valid(p[12]);
}
static bool configuring(uint8_t signal) { return signal == 3 || signal == 5 || signal == 4; }
bool selection_valid(const uint8_t *p, size_t n) {
    return p && n == 43 && p[0] == 42 && vendor(p + 1, n - 1, 0xad) &&
           ((p[9] & 0xf8) == 0x10 || (p[9] & 0xf8) == 0xa0 ||
            (kR2 == 3 && (p[9] & 0xf8) == 0x40)) &&
           (p[10] == 8 || p[10] == 2);
}
struct Peer {
    uint8_t cie[43];
    uint8_t selection;
    uint8_t used;
};
static Peer peers[64];
static uint8_t peers_lock;
static uint32_t features(const uint8_t *c) {
    return uint32_t(c[19]) | (uint32_t(c[20]) << 8) | (uint32_t(c[21]) << 16) |
           (uint32_t(c[22]) << 24);
}
static unsigned raw_caps(const uint8_t *c) {
    unsigned r = (c[9] & 0x10) ? 2 : 0;
    if (kR2 && c[18] == 1 && (c[9] & 0xa0))
        r |= 8;
    if (kR2 == 3 && c[18] == 1 && (features(c) & 0x80) && (c[9] & 0x40))
        r |= 1;
    return r;
}
unsigned proxy_caps(uint8_t slot) {
    return slot < 64 && __atomic_load_n(&peers[slot].used, __ATOMIC_ACQUIRE)
               ? raw_caps(peers[slot].cie)
               : 0;
}
bool proxy_is_selection(uint8_t slot) {
    return slot < 64 && __atomic_load_n(&peers[slot].used, __ATOMIC_ACQUIRE) &&
           peers[slot].selection;
}
bool proxy_r22(uint8_t slot) {
    return slot < 64 && __atomic_load_n(&peers[slot].used, __ATOMIC_ACQUIRE) &&
           peers[slot].cie[18] == 1 && (features(peers[slot].cie) & 0x80);
}
static int peer_slot(const uint8_t *c, bool selection) {
    while (__atomic_test_and_set(&peers_lock, __ATOMIC_ACQUIRE)) {
    }
    int available = -1, found = -1;
    for (unsigned i = 0; i < 64; i++) {
        if (!peers[i].used) {
            if (available < 0)
                available = i;
            continue;
        }
        if (peers[i].selection != selection)
            continue;
        unsigned j = 0;
        for (; j < 43 && peers[i].cie[j] == c[j]; j++) {
        }
        if (j == 43) {
            found = i;
            break;
        }
    }
    if (found < 0 && available >= 0) {
        found = available;
        copy(peers[found].cie, c, 43);
        peers[found].selection = selection;
        __atomic_store_n(&peers[found].used, 1, __ATOMIC_RELEASE);
    }
    __atomic_clear(&peers_lock, __ATOMIC_RELEASE);
    return found;
}
bool proxy_choose(uint8_t slot, int rate, int mode, uint8_t *flags) {
    unsigned caps = proxy_caps(slot);
    if (!caps || !flags || (mode != 4096 && mode != 8192 && !(kR2 == 3 && mode == 12288)))
        return false;
    if (mode == 12288 && (caps & 1))
        rate = 44100;
    unsigned want = rate == 96000 ? 8 : rate == 44100 ? 1 : 2;
    unsigned pick = (caps & want) ? want : (caps & 2) ? 2 : (caps & 8) ? 8 : 1;
    bool lossless = (mode == 12288) && (pick == 1) && proxy_r22(slot);
    *flags = (pick == 8 ? 1 : pick == 1 ? 2 : 0) | (mode == 8192 ? 4 : lossless ? 8 : 0);
    return true;
}
bool negotiated_stream_valid(const uint8_t *s) {
    if (!s)
        return false;
    if (s[0] == 0) {
        if (s[1] != 0xaa)
            return false;
        for (unsigned i = 2; i < 11; i++)
            if (s[i])
                return false;
        return true;
    }
    if (!kR2 || s[0] != 1 || s[2] || s[3] || s[4] != 0x0f || s[5] != 2 || s[6] != 3 || s[7] != 3 ||
        s[8] != 3 || s[9] || s[10] != 0xaa)
        return false;
    unsigned high = s[1] & 0xf0, low = s[1] & 15;
    if (kR2 == 3)
        return (high == 0x10 || high == 0x90) && low <= 7;
    if (kR2 == 2)
        return (high == 0 || high == 0x80) && low <= 3;
    return (high == 0 || high == 0x80) && low == 0;
}
static void wire(uint8_t *dst, bool config) {
    for (size_t i = 0; i < 43; i++)
        dst[i] = 0;
    dst[0] = 42;
    dst[1] = 0;
    dst[2] = 255;
    dst[3] = 0xd7;
    dst[7] = 0xad;
    dst[9] = config ? (current_rate() == 96000   ? 0xa0
                       : current_rate() == 44100 ? 0x40
                                                 : 0x10)
                    : (kR2 == 3 ? 0xf0 : 0xb0);
    dst[10] = 8;
    dst[11] = 0x50;
    dst[12] = 0x64;
    dst[13] = 0x64;
    dst[14] = 0x64;
    dst[15] = 255;
    dst[16] = 255;
    for (unsigned i = 0; i < 11; i++)
        dst[18 + i] = kStream[i];
    if (!config && kR2)
        dst[19] = kR2 == 3 ? 0x17 : kR2 == 2 ? 3 : 0;
}
bool proxy_selection(const uint8_t *p, uint8_t *out) {
    if (!proxy_codec(p) || !out || !proxy_caps(p[12]))
        return false;
    uint8_t flags;
    if (!proxy_choose(p[12], flags_rate(p[13]), flags_mode(p[13]), &flags) || flags != p[13])
        return false;
    const uint8_t *cap = peers[p[12]].cie;
    wire(out, true);
    out[9] = flags_rate(flags) == 96000 ? 0xa0 : flags_rate(flags) == 44100 ? 0x40 : 0x10;
    out[10] = (cap[10] & 8) ? 8 : (cap[10] & 2) ? 2 : 8;
    if (!kR2 || cap[18] == 0) {
        const uint8_t r1[11] = {0, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        copy(out + 18, r1, 11);
    } else {
        uint8_t source = kR2 == 3 ? 0x17 : kR2 == 2 ? 3 : 0;
        uint32_t f = features(cap);
        uint8_t peer = f & 255;
        out[18] = 1;
        out[19] = ((peer | source) & 0xf0) | ((peer & source) & 15);
        out[20] = (f >> 8) & 255;
        out[21] = (f >> 16) & 255;
        out[22] = (f >> 24) & 255;
    }
    return negotiated_stream_valid(out + 18);
}
static Result translate(const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint8_t signal,
                        State *state, bool outgoing) {
    if ((n && !in) || (!out && cap) || !state)
        return {Error::Truncated, 0, false};
    // Validate every TLV and calculate exact size before writing or changing state.
    size_t total = 0;
    bool changed = false;
    unsigned media = 0;
    const uint8_t *ad = nullptr;
    int slot = -1;
    uint8_t in_flags = 0;
    for (size_t p = 0; p < n;) {
        if (n - p < 2)
            return {Error::Truncated, 0, false};
        size_t len = in[p + 1];
        if (len > n - p - 2)
            return {Error::Truncated, 0, false};
        size_t add = len + 2;
        if (in[p] == 7) {
            if (++media > 1)
                return {Error::DuplicateCodec, 0, false};
            if (outgoing && vendor(in + p + 2, len, 0x24) && marked_payload(in + p + 2, len)) {
                if (len != 13 || ((in[p + 10] & 0x30) == 0) ||
                    (configuring(signal) && (in[p + 10] & 0xf0) != 0x10 &&
                     (in[p + 10] & 0xf0) != 0x20) ||
                    (in[p + 10] & 15) != 2)
                    return {Error::Unsupported, 0, false};
                if (configuring(signal)) {
                    uint8_t proxy[14], selected[43];
                    proxy[0] = 13;
                    copy(proxy + 1, in + p + 2, 13);
                    if (!proxy_selection(proxy, selected))
                        return {Error::Unsupported, 0, false};
                }
                add = 44;
                changed = true;
            } else if (!outgoing && vendor(in + p + 2, len, 0xad)) {
                if (len != 42 || ((in[p + 10] & 0xf0) == 0) || (in[p + 11] & 0x0a) == 0)
                    return {Error::Unsupported, 0, false};
                uint8_t c[43];
                c[0] = 42;
                copy(c + 1, in + p + 2, 42);
                if (configuring(signal) && !selection_valid(c, 43))
                    return {Error::Unsupported, 0, false};
                if (!raw_caps(c) || c[18] > 1)
                    return {Error::Unsupported, 0, false};
                ad = in + p + 2;
                add = 15;
                changed = true;
            } else if (!outgoing && vendor(in + p + 2, len, 0x24) &&
                       marked_payload(in + p + 2, len)) {
                // Never accept a remote codec that forges our private local proxy marker.
                return {Error::Unsupported, 0, false};
            }
        }
        if (total > cap || add > cap - total)
            return {Error::Capacity, 0, false};
        total += add;
        p += len + 2;
    }
    if (ad) {
        uint8_t c[43];
        c[0] = 42;
        copy(c + 1, ad, 42);
        slot = peer_slot(c, configuring(signal));
        if (slot < 0 || !proxy_choose((uint8_t)slot, current_rate(), current_mode(), &in_flags))
            return {Error::Unsupported, 0, false};
        uint8_t proxy[14] = {13, 0, 255, 0xd7, 0, 0, 0, 0x24, 0, 0x12, 0, 0, 0, 0};
        set_proxy(proxy, (uint8_t)slot, in_flags);
        uint8_t selected[43];
        if (!proxy_selection(proxy, selected))
            return {Error::Unsupported, 0, false};
    }
    size_t q = 0;
    State next = *state;
    for (size_t p = 0; p < n;) {
        size_t len = in[p + 1];
        if (in[p] == 7 && outgoing && vendor(in + p + 2, len, 0x24) &&
            marked_payload(in + p + 2, len)) {
            uint8_t c[43];
            if (configuring(signal)) {
                uint8_t proxy[14];
                proxy[0] = 13;
                copy(proxy + 1, in + p + 2, 13);
                proxy_selection(proxy, c);
            } else
                wire(c, false);
            out[q++] = 7;
            out[q++] = 42;
            copy(out + q, c + 1, 42);
            q += 42;
            if (configuring(signal)) {
                copy(next.selected, c, 43);
                next.selected_seen = 1;
            }
        } else if (in[p] == 7 && !outgoing && vendor(in + p + 2, len, 0xad)) {
            out[q++] = 7;
            out[q++] = 13;
            const uint8_t hd[] = {0, 255, 0xd7, 0, 0, 0, 0x24, 0, 0x12, 0xa5, 0xad, 0, 0};
            copy(out + q, hd, 13);
            out[q + 11] = (uint8_t)slot;
            out[q + 12] = in_flags;
            out[q + 8] = configuring(signal) ? (proxy_rate() == 1 ? 0x22 : 0x12) : 0x32;
            q += 13;
            uint8_t c[43];
            c[0] = 42;
            copy(c + 1, in + p + 2, 42);
            copy(next.peer, c, 43);
            next.peer_seen = 1;
            if (configuring(signal)) {
                copy(next.selected, c, 43);
                next.selected_seen = 1;
            }
        } else {
            copy(out + q, in + p, len + 2);
            q += len + 2;
        }
        p += len + 2;
    }
    *state = next;
    return {Error::Ok, q, changed};
}
Result incoming(const uint8_t *i, size_t n, uint8_t *o, size_t c, uint8_t s, State *t) {
    return translate(i, n, o, c, s, t, false);
}
Result outgoing(const uint8_t *i, size_t n, uint8_t *o, size_t c, uint8_t s, State *t) {
    return translate(i, n, o, c, s, t, true);
}
size_t qti_start(const uint8_t *p, size_t n, const State *s, uint8_t *out, size_t cap) {
    if (!p || n != 57 || p[0] != 1 || p[1] != 0x40 || p[2] || p[3] || p[4] || !s ||
        !s->selected_seen || !selection_valid(s->selected, 43) || !out || cap < 66)
        return 0;
    unsigned mtu = unsigned(p[23]) | (unsigned(p[24]) << 8);
    if (mtu > 65523)
        return 0;
    for (unsigned i = 0; i < 66; i++)
        out[i] = 0;
    out[0] = 0x0a;
    out[1] = 10;
    out[3] = 10; // start, codec Adaptive, Slimbus, stream Adaptive
    out[7] = p[7];
    out[8] = 2;
    // Derive the transport clock from the negotiated wire CIE, never the synthetic HD clock.
    uint8_t rate = s->selected[9] & 0xf8;
    out[9] = rate == 0xa0 ? 8 : rate == 0x40 ? 1 : 2;
    out[10] = 0;
    copy(out + 11, p + 19, 4);
    mtu += 12;
    out[15] = mtu & 255;
    out[16] = mtu >> 8;
    out[17] = 1;
    out[20] = 150;
    out[21] = 0;
    copy(out + 22, s->selected, 43);
    return 66;
}
} // namespace s25
