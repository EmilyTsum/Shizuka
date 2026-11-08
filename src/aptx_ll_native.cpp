// SPDX-License-Identifier: GPL-2.0-only
#include "aptx_ll_native.h"
#ifndef S25_HAL_ONLY
extern "C" __attribute__((visibility("hidden"))) void s25_anchor();
extern "C" __attribute__((visibility("hidden"))) void *memcpy(void *, const void *, size_t);
extern "C" __attribute__((visibility("hidden"))) void *memset(void *, int, size_t);
namespace s25::ll {
static uintptr_t base() { return (uintptr_t)&s25_anchor - 0x2000000; }
struct Peer {
    uint8_t cie[20];
    uint8_t size;
    bool selection, used;
};
static Peer peers[64];
static uint8_t pool_lock;
static uintptr_t table[13];
static uint8_t table_lock;
static Plan selected;
static bool selected_valid;
static uint8_t selected_lock;
struct Lock {
    uint8_t *p;
    Lock(uint8_t *value) : p(value) {
        while (__atomic_test_and_set(p, __ATOMIC_ACQUIRE)) {
        }
    }
    ~Lock() { __atomic_clear(p, __ATOMIC_RELEASE); }
};
unsigned capabilities(uint8_t slot) {
    if (slot == 255)
        return 3;
    if (slot >= 64 || !__atomic_load_n(&peers[slot].used, __ATOMIC_ACQUIRE))
        return 0;
    return ((peers[slot].cie[9] & 0x20) ? 1 : 0) | ((peers[slot].cie[9] & 0x10) ? 2 : 0);
}
bool selection_slot(uint8_t slot) {
    return slot < 64 && __atomic_load_n(&peers[slot].used, __ATOMIC_ACQUIRE) &&
           peers[slot].selection;
}
static int remember(const uint8_t *c, size_t n, bool selection) {
    Capabilities parsed{};
    if (parse(c, n, &parsed) != Error::Ok)
        return -1;
    Lock lock(&pool_lock);
    int empty = -1;
    for (unsigned i = 0; i < 64; i++) {
        if (!peers[i].used) {
            if (empty < 0)
                empty = i;
            continue;
        }
        if (peers[i].size != n || peers[i].selection != selection)
            continue;
        size_t j = 0;
        for (; j < n && peers[i].cie[j] == c[j]; j++) {
        }
        if (j == n)
            return i;
    }
    if (empty < 0)
        return -1;
    auto &p = peers[empty];
    memcpy(p.cie, c, n);
    p.size = n;
    p.selection = selection;
    __atomic_store_n(&p.used, true, __ATOMIC_RELEASE);
    return empty;
}
static void mark(uint8_t *out, uint8_t slot, uint8_t rate) {
    memset(out, 0, 20);
    out[0] = 13;
    out[2] = 255;
    out[3] = 0xd7;
    out[7] = 0x24;
    out[9] = rate == 1   ? 0x22
             : rate == 2 ? 0x12
                         : uint8_t(((capabilities(slot) & 1) ? 0x20 : 0) |
                                   ((capabilities(slot) & 2) ? 0x10 : 0) | 2);
    out[10] = 0xa5;
    out[11] = 0x4c;
    out[12] = slot;
    out[13] = rate;
}
bool user_peer(void *codec, const uint8_t *peer, uint8_t *out) {
    if (!codec || !out)
        return false;
    if (proxy(peer)) {
        memcpy(out, peer, 20);
        return true;
    }
    // Samsung passes the currently negotiated codec during user switches.
    // Use the LL capabilities stored in THIS peer's codec object, never a global last peer.
    uint64_t saved = *(uint64_t *)((uint8_t *)codec + 0x110);
    uint8_t slot = (saved >> 8) & 255;
    if ((saved & 0xffffffff00000000ULL) != kDescriptor || slot >= 64 || !capabilities(slot) ||
        selection_slot(slot))
        return false;
    mark(out, slot, 0);
    return true;
}
void normal_cie(const uint8_t *c, uint8_t *out) {
    memset(out, 0, 20);
    out[0] = 9;
    out[2] = 255;
    out[3] = 0x4f;
    out[7] = 1;
    out[9] = c[9];
}
bool make_wire(const uint8_t *c, uint8_t *out, size_t *size) {
    if (!proxy(c) || !out || !size)
        return false;
    uint8_t local[20] = {19, 0, 255, 0x0a, 0,   0, 0,  2, 0,   0x32,
                         2,  0, 180, 0,    104, 1, 50, 1, 180, 0};
    const uint8_t *raw = local;
    size_t n = 20;
    if (c[12] < 64) {
        if (!capabilities(c[12]))
            return false;
        raw = peers[c[12]].cie;
        n = peers[c[12]].size;
    }
    if (!c[13]) {
        memcpy(out, raw, n);
        *size = n;
        return true;
    }
    Plan plan{};
    if (negotiate(raw, n, {c[13] == 1 ? 44100u : 48000u, 16, false, 65535}, &plan) != Error::Ok)
        return false;
    memcpy(out, plan.wire, plan.wire_size);
    *size = plan.wire_size;
    return true;
}
bool translate(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *size, bool outgoing,
               uint8_t signal) {
    if ((!in && n) || !out || !size)
        return false;
    bool ll_configuration = false;
    if (outgoing)
        for (size_t p = 0; p < n;) {
            if (n - p < 2 || size_t(in[p + 1]) + 2 > n - p)
                return false;
            if (in[p] == 7 && in[p + 1] == 13) {
                uint8_t c[20]{};
                c[0] = 13;
                memcpy(c + 1, in + p + 2, 13);
                if (proxy(c) && c[13])
                    ll_configuration = true;
            }
            p += size_t(in[p + 1]) + 2;
        }
    size_t q = 0;
    for (size_t p = 0; p < n;) {
        if (n - p < 2 || size_t(in[p + 1]) + 2 > n - p)
            return false;
        size_t len = in[p + 1];
        uint8_t cie[20]{};
        bool match = false;
        // LL endpoints may omit AVDTP Delay Reporting even when the ordinary aptX endpoint supports
        // it. It is optional: never inherit its negotiated service from the previous codec.
        if (ll_configuration && in[p] == 8) {
            p += len + 2;
            continue;
        }
        if (in[p] == 7 && len <= 19) {
            cie[0] = len;
            memcpy(cie + 1, in + p + 2, len);
            if (outgoing && proxy(cie)) {
                size_t wire_size = 0;
                uint8_t wire[20];
                if (!make_wire(cie, wire, &wire_size) || q + wire_size + 1 > cap)
                    return false;
                out[q++] = 7;
                out[q++] = wire_size - 1;
                memcpy(out + q, wire + 1, wire_size - 1);
                q += wire_size - 1;
                match = true;
            } else if (!outgoing && len >= 8 && !cie[1] && cie[2] == 255 &&
                       (cie[3] == 0x0a || cie[3] == 0xd7) && !cie[4] && !cie[5] && !cie[6] &&
                       cie[7] == 2 && !cie[8]) {
                bool sel = signal == 3 || signal == 4 || signal == 5;
                int slot = remember(cie, len + 1, sel);
                if (slot < 0 || q + 15 > cap)
                    return false;
                uint8_t proxy_cie[20];
                mark(proxy_cie, slot,
                     sel ? ((cie[9] & 0x30) == 0x20   ? 1
                            : (cie[9] & 0x30) == 0x10 ? 2
                                                      : 0)
                         : 0);
                if (sel && !proxy_cie[13])
                    return false;
                out[q++] = 7;
                out[q++] = 13;
                memcpy(out + q, proxy_cie + 1, 13);
                q += 13;
                match = true;
            }
        }
        if (!match) {
            if (q + len + 2 > cap)
                return false;
            memcpy(out + q, in + p, len + 2);
            q += len + 2;
        }
        p += len + 2;
    }
    *size = q;
    return true;
}
static uint64_t descriptor(uint8_t slot, uint8_t rate = 0) {
    return kDescriptor | (uint64_t(slot) << 8) | rate;
}
static void pod(uint8_t *p, unsigned rates, uint64_t d) {
    int priority = *(int *)(p + 4);
    memset(p, 0, 64);
    *(int *)p = kPrivateIndex;
    *(int *)(p + 4) = priority;
    *(uint32_t *)(p + 8) = rates;
    *(uint32_t *)(p + 12) = 1;
    *(uint32_t *)(p + 16) = 2;
    *(uint64_t *)(p + 48) = d;
}
void fix_object(void *codec) {
    auto *p = (uint8_t *)codec;
    *(uint32_t *)(p + 0x30) = kPrivateIndex;
    *(uint64_t *)(p + 0x38) = kPublicId;
    const char name[] = "aptX LL";
    p[0x40] = 14;
    memcpy(p + 0x41, name, sizeof name);
    for (unsigned off = 0x60; off <= 0x160; off += 64)
        *(uint32_t *)(p + off) = kPrivateIndex;
    *(uint32_t *)(p + 0xa8) = 3;
    *(uint32_t *)(p + 0xac) = 1;
    *(uint32_t *)(p + 0xb0) = 2;
}
static uint8_t set(void *codec, const uint8_t *peer, bool capability, uint8_t *out) {
    if (!proxy(peer))
        return 0x29;
    auto *p = (uint8_t *)codec;
    uint8_t slot = peer[12];
    uint64_t saved = *(uint64_t *)(p + 0x110);
    uint8_t saved_slot = (saved >> 8) & 255;
    bool cached = (saved & 0xffffffff00000000ULL) == kDescriptor && saved_slot < 64 &&
                  capabilities(saved_slot) && !selection_slot(saved_slot);
    if (capability && slot < 64 && cached && peers[saved_slot].cie[3] == 0x0a &&
        peers[slot].cie[3] == 0xd7) {
        unsigned requested = *(uint32_t *)(p + 0x128);
        if (!requested || (capabilities(saved_slot) & requested))
            return 0xc4; // Prefer the peer's canonical CSR endpoint; keep D7-only devices working.
    }
    if (selection_slot(slot) && cached && peers[saved_slot].cie[3] == peers[slot].cie[3])
        slot = saved_slot;
    unsigned rates = capabilities(slot);
    if (!rates)
        return 0x29;
    unsigned requested = *(uint32_t *)(p + 0x128), bits = *(uint32_t *)(p + 0x12c),
             channels = *(uint32_t *)(p + 0x130);
    if ((requested & ~3u) || (bits && bits != 1) || (channels && channels != 2))
        return 0xc4;
    unsigned chosen = 0;
    if (!capability && peer[13]) {
        chosen = peer[13];
        if (requested && !(requested & chosen))
            return 0xc4;
    } else if (requested) {
        chosen = (requested & rates & 2) ? 2 : (requested & rates & 1) ? 1 : 0;
    } else
        chosen = (rates & 2) ? 2 : 1;
    if (!chosen || !(rates & chosen))
        return 0xc4;
    fix_object(codec);
    pod(p + 0x60, chosen, descriptor(slot, chosen));
    pod(p + 0xa0, 3, 0);
    pod(p + 0xe0, rates, descriptor(slot));
    mark(p + 0x1a0, slot, chosen);
    mark(p + 0x1b4, slot, 0);
    mark(p + 0x1c8, slot, 0);
    if (out)
        memcpy(out, p + 0x1a0, 20);
    return 0;
}
static bool peer(void *codec, const uint8_t *c) {
    if (!proxy(c) || c[12] >= 64 || !capabilities(c[12]))
        return false;
    auto *p = (uint8_t *)codec;
    fix_object(codec);
    uint64_t saved = *(uint64_t *)(p + 0x110);
    uint8_t slot = c[12], old = (saved >> 8) & 255;
    if ((saved & 0xffffffff00000000ULL) == kDescriptor && old < 64 && capabilities(old) &&
        !selection_slot(old)) {
        if (selection_slot(slot) && peers[old].cie[3] == peers[slot].cie[3])
            slot = old;
        else if (!selection_slot(slot) && peers[old].cie[3] == 0x0a && peers[slot].cie[3] == 0xd7 &&
                 capabilities(old) == capabilities(slot))
            slot = old;
    }
    pod(p + 0xe0, capabilities(slot), descriptor(slot));
    mark(p + 0x1c8, slot, 0);
    return true;
}
void *factory(int priority) {
    auto allocate = (void *(*)(size_t))(base() + 0xe62580);
    void *codec = allocate(0x1e0);
    if (!codec)
        return nullptr;
    ((void (*)(void *, int))(base() + 0x972f58))(codec, priority == 0 ? 1800 : priority);
    {
        Lock lock(&table_lock);
        if (!table[2]) {
            auto *old = *(uintptr_t **)codec;
            for (int i = -2; i < 11; i++)
                table[i + 2] = old[i];
            table[7] = (uintptr_t)&set;
            table[10] = (uintptr_t)&peer;
        }
    }
    *(uintptr_t **)codec = table + 2;
    fix_object(codec);
    auto *p = (uint8_t *)codec;
    pod(p + 0xa0, 3, 0);
    mark(p + 0x1b4, 255, 0);
    return codec;
}
static bool view(void *codec, uint8_t *storage) {
    auto *p = (uint8_t *)codec;
    if (!proxy(p + 0x1a0))
        return false;
    ((void (*)(void *, int))(base() + 0x972f58))(storage, *(int *)(p + 0x58));
    for (unsigned off = 0x60; off <= 0x160; off += 64) {
        memcpy(storage + off, p + off, 64);
        *(uint32_t *)(storage + off) = 2;
        *(uint64_t *)(storage + off + 48) = 0;
    }
    for (unsigned off = 0x1a0; off <= 0x1c8; off += 20)
        normal_cie(p + off, storage + off);
    return true;
}
static void destroy(uint8_t *p) { ((void (*)(void *))(*(uintptr_t **)p)[0])(p); }
static bool activate(void *codec) {
    auto *c = (uint8_t *)codec + 0x1a0;
    uint8_t raw[20];
    size_t n;
    if (!make_wire(c, raw, &n) || !c[13])
        return false;
    Plan plan{};
    if (negotiate(raw, n, {c[13] == 1 ? 44100u : 48000u, 16, false, 65535}, &plan) != Error::Ok)
        return false;
    Lock lock(&selected_lock);
    selected = plan;
    selected_valid = true;
    return true;
}
bool hal_view(void *codec, uint8_t *out, bool (*convert)(uint8_t *, void *)) {
    alignas(16) uint8_t storage[0x1e0]{};
    if (!view(codec, storage))
        return false;
    bool ok = convert(out, storage);
    destroy(storage);
    return ok && activate(codec);
}
bool specific_view(void *codec, uint8_t *out, bool (*convert)(void *, uint8_t *)) {
    alignas(16) uint8_t storage[0x1e0]{};
    if (!view(codec, storage))
        return false;
    bool ok = convert(storage, out);
    destroy(storage);
    if (ok) {
        *(uint32_t *)out = 4;
        *(uint32_t *)(out + 8) = ((uint8_t *)codec)[0x1ad] == 1 ? 1 : 2;
        out[12] = 1;
    }
    return ok && activate(codec);
}
// Hash-gated Samsung BtaAvCoPeer ABI: address[6], 19 sink records of26 bytes,
// num_sup_sinks+0x3e7, selected sink pointer+0x3f0, current codec+0x400.
static unsigned wire_vendor(const uint8_t *c) {
    return proxy(c) && c[12] < 64 && capabilities(c[12]) ? peers[c[12]].cie[3] : 0;
}
void prioritize_peer(void *context) {
    if (!context)
        return;
    auto *p = (uint8_t *)context;
    unsigned count = p[0x3e7];
    if (count > 19)
        return;
    unsigned first = count, primary = count;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *c = p + 8 + 26 * i;
        if (!proxy(c))
            continue;
        if (first == count)
            first = i;
        if (wire_vendor(c) == 0x0a) {
            primary = i;
            break;
        }
    }
    if (primary == count || primary == first)
        return;
    uint8_t *a = p + 6 + 26 * first, *b = p + 6 + 26 * primary, temp[26];
    memcpy(temp, a, 26);
    memcpy(a, b, 26);
    memcpy(b, temp, 26);
    auto &selected_sink = *(uint8_t **)(p + 0x3f0);
    if (selected_sink == a)
        selected_sink = b;
    else if (selected_sink == b)
        selected_sink = a;
}
bool endpoint_index(const void *context, uint8_t *out) {
    if (!context || !out)
        return false;
    auto *p = (const uint8_t *)context;
    unsigned count = p[0x3e7];
    if (count > 19)
        return false;
    const uint8_t *current = p + 0x400;
    unsigned vendor = wire_vendor(current);
    if (!vendor)
        return false;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *c = p + 8 + 26 * i;
        if (wire_vendor(c) == vendor && (c[9] & current[9] & 0x30)) {
            *out = p[6 + 26 * i];
            return true;
        }
    }
    return false;
}
void deactivate() {
    Lock lock(&selected_lock);
    selected_valid = false;
}
bool limit_hal(uint8_t *config) {
    if (!config || *(uint32_t *)config != 3)
        return false;
    Lock lock(&selected_lock);
    if (!selected_valid || *(uint32_t *)(config + 0x18) != selected.rate || config[0x1d] != 16)
        return false;
    // SehCodecConfiguration.peerMtu at +8, verified in Samsung setup_codec (0x8564d4).
    uint32_t mtu = *(uint32_t *)(config + 8);
    if (mtu < 4)
        return false;
    *(uint32_t *)(config + 8) = mtu < selected.packet_bytes ? (mtu & ~3u) : selected.packet_bytes;
    return true;
}
bool offload_start(const uint8_t *in, size_t n, uint8_t *out) {
    if (!in || !out || n != 57 || in[0] != 1 || in[1] != 4 || in[2] || in[3] || in[4])
        return false;
    Lock lock(&selected_lock);
    if (!selected_valid)
        return false;
    unsigned mtu = unsigned(in[23]) | (unsigned(in[24]) << 8);
    if (mtu < 4)
        return false;
    unsigned limited = mtu < selected.packet_bytes ? mtu & ~3u : selected.packet_bytes;
    if (!limited)
        return false;
    memcpy(out, in, n);
    out[23] = limited;
    out[24] = limited >> 8;
    return true;
}
} // namespace s25::ll
#endif

#ifndef S25_HAL_ONLY
extern "C" void tramp_ll_selectable(void *, void *);
extern "C" uint8_t tramp_ll_sep_index(const uint8_t *);
extern "C" void s25_ll_selectable(void *self, void *peer) {
    s25::ll::prioritize_peer(peer);
    tramp_ll_selectable(self, peer);
}
extern "C" uint8_t s25_ll_sep_index(const uint8_t *address) {
    uintptr_t b = (uintptr_t)&s25_anchor - 0x2000000;
    auto find = (void *(*)(void *, const uint8_t *))(b + 0x654d68);
    void *cache = *(void **)(b + 0xf14368);
    uint8_t index;
    void *peer = cache && address ? find(cache, address) : nullptr;
    return s25::ll::endpoint_index(peer, &index) ? index : tramp_ll_sep_index(address);
}
#endif
