// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace s25::ll {
// All calls, including alarm callbacks, run on the Bluetooth main loop.
struct SwitchOps {
    void *(*create_alarm)();
    void (*schedule)(void *, uint64_t, void (*)(void *), void *);
    bool (*send)(void *, void *);
    void (*release)(void *);
};
class SwitchGate {
    struct Node {
        SwitchGate *owner{};
        void *ccb{};
        void *alarm{};
        void *buffer{};
        void *command{};
        uint8_t address[6]{};
        bool last_ll{}, closed{};
    } nodes_[8]{};
    SwitchOps ops_{};
    static bool address_matches(const Node &n) {
        auto *p = (uint8_t *)n.ccb;
        for (unsigned i = 0; i < 6; i++)
            if (p[i] != n.address[i])
                return false;
        return true;
    }
    static void resume(void *context) {
        auto &n = *(Node *)context;
        auto &self = *n.owner;
        void *buffer = n.buffer;
        if (!buffer)
            return;
        auto *p = (uint8_t *)n.ccb;
        // The command copy belongs to AVDTP; only the held original belongs to us.
        if (p[0x15c0] && address_matches(n) && *(void **)(p + 0x15a8) == n.command) {
            if (*(void **)(p + 0x15b0)) {
                self.ops_.schedule(n.alarm, 20, resume, &n);
                return;
            }
            n.buffer = nullptr;
            self.ops_.send(n.ccb, buffer);
        } else {
            n.buffer = nullptr;
            self.ops_.release(buffer);
        }
    }
    static bool ll_config(const uint8_t *p, size_t len) {
        if (len < 2)
            return false;
        for (size_t i = 2; i + 2 <= len;) {
            size_t n = p[i + 1];
            if (n > len - i - 2)
                return false;
            if (p[i] == 7 && n >= 8) {
                auto *c = p + i + 2;
                if (c[0] == 0 && c[1] == 255 && (c[2] == 0x0a || c[2] == 0xd7) && !c[3] && !c[4] &&
                    !c[5] && c[6] == 2 && !c[7])
                    return true;
            }
            i += n + 2;
        }
        return false;
    }

    static bool hd_config(const uint8_t *p, size_t len) {
        if (len < 2)
            return false;
        for (size_t i = 2; i + 2 <= len;) {
            size_t n = p[i + 1];
            if (n > len - i - 2)
                return false;
            if (p[i] == 7 && n == 13) {
                auto *c = p + i + 2;
                // Genuine aptX HD only. Adaptive's private HD-shaped proxy is
                // translated before this hook, but must never match if present.
                return c[0] == 0 && c[1] == 255 && c[2] == 0xd7 && !c[3] && !c[4] && !c[5] &&
                       c[6] == 0x24 && !c[7] && !(c[9] == 0xa5 && c[10] == 0xad);
            }
            i += n + 2;
        }
        return false;
    }

  public:
    constexpr SwitchGate() = default;
    constexpr explicit SwitchGate(SwitchOps ops) : ops_(ops) {}
    void bind(SwitchOps ops) { ops_ = ops; }
    bool send(void *ccb, void *buffer) {
        if (!ccb || !buffer)
            return ops_.send(ccb, buffer);
        auto *p = (uint8_t *)ccb;
        auto *h = (uint16_t *)buffer;
        const unsigned event = h[0], type = h[3] & 15;
        if (type != 0)
            return ops_.send(ccb, buffer); // Responses must never wait behind a command.
        Node *n = nullptr;
        for (auto &item : nodes_)
            if (item.ccb == ccb) {
                n = &item;
                break;
            }
        if (!n)
            for (auto &item : nodes_)
                if (!item.ccb) {
                    n = &item;
                    n->ccb = ccb;
                    n->owner = this;
                    break;
                }
        if (!n)
            return ops_.send(ccb, buffer);
        if (n->buffer) {
            ops_.release(n->buffer);
            n->buffer = nullptr;
        } // A newer command supersedes an unsent one.
        if (!address_matches(*n)) {
            for (unsigned i = 0; i < 6; i++)
                n->address[i] = p[i];
            n->last_ll = false;
            n->closed = false;
        }
        if (event == 8)
            n->closed = true;
        if (event != 3)
            return ops_.send(ccb, buffer);
        bool next_ll = ll_config((uint8_t *)buffer + 8 + h[2], h[1]);
        bool next_hd = hd_config((uint8_t *)buffer + 8 + h[2], h[1]);
        bool delay = n->closed && (n->last_ll || next_ll || next_hd);
        n->last_ll = next_ll;
        n->closed = false;
        // No command timer has started yet. Let media L2CAP teardown run on the same
        // main loop before the next SetConfiguration (observed response ~126 ms).
        // BT-B10 also rejects genuine HD with BAD_STATE when SetConfiguration
        // precedes the old media L2CAP disconnection response. Its identical HD
        // configuration succeeds after teardown; reuse this bounded guard.
        if (delay && !n->buffer && p[0x15c0] && *(void **)(p + 0x15a8)) {
            if (!n->alarm)
                n->alarm = ops_.create_alarm();
            if (n->alarm) {
                n->buffer = buffer;
                n->command = *(void **)(p + 0x15a8);
                ops_.schedule(n->alarm, 500, resume, n);
                return false;
            }
        }
        return ops_.send(ccb, buffer);
    }
};
} // namespace s25::ll
