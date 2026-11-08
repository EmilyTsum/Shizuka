// SPDX-License-Identifier: GPL-2.0-only
#include "../src/aptx_ll_switch.h"
#include <assert.h>
#include <stdio.h>
using namespace s25::ll;
static unsigned sent, freed;
static void (*callback)(void *);
static void *context;
static bool fail_alarm;
static unsigned schedules;
static uint64_t last_delay;
static void *alarm() { return fail_alarm ? nullptr : (void *)1; }
static void schedule(void *, uint64_t ms, void (*cb)(void *), void *ctx) {
    assert(ms == 500 || ms == 20);
    schedules++;
    last_delay = ms;
    callback = cb;
    context = ctx;
}
static bool send(void *, void *) {
    sent++;
    return false;
}
static void release(void *) { freed++; }
struct alignas(8) Ccb {
    uint8_t b[0x15d0]{};
    Ccb() {
        b[0] = 42;
        b[0x15c0] = 1;
        *(void **)(b + 0x15a8) = (void *)123;
    }
};
struct alignas(8) Message {
    uint16_t event, len, offset, type;
    uint8_t data[32]{};
    Message(unsigned ev, bool ll = false, unsigned response = 0)
        : event(ev), len(14), offset(0), type(response) {
        uint8_t a[] = {24, 8, 1, 0, 7, 8, 0, 255, 0x0a, 0, 0, 0, 2, 0};
        for (unsigned i = 0; i < 14; i++)
            data[i] = a[i];
        if (!ll)
            data[12] = 1;
    }
};
int main() {
    SwitchOps ops{alarm, schedule, send, release};
    SwitchGate g(ops);
    Ccb c;
    Message normal(3), close(8), ll(3, true), rsp(3, true, 2);
    g.send(c.b, &normal);
    assert(sent == 1);
    g.send(c.b, &close);
    g.send(c.b, &ll);
    assert(sent == 2 && callback);
    g.send(c.b, &rsp);
    assert(sent == 3);
    *(void **)(c.b + 0x15b0) = (void *)777;
    callback(context);
    assert(sent == 3);
    *(void **)(c.b + 0x15b0) = nullptr;
    callback(context);
    assert(sent == 4 && !freed); // Responses run during deferral.
    g.send(c.b, &close);
    g.send(c.b, &normal);
    assert(sent == 5);
    callback(context);
    assert(sent == 6); // LL -> normal.
    g.send(c.b, &close);
    g.send(c.b, &ll);
    c.b[0x15c0] = 0;
    callback(context);
    assert(freed == 1 && sent == 7);
    c.b[0x15c0] = 1;
    g.send(c.b, &close);
    g.send(c.b, &normal);
    *(void **)(c.b + 0x15a8) = (void *)456;
    callback(context);
    assert(freed == 2 && sent == 8);
    g.send(c.b, &close);
    g.send(c.b, &ll);
    g.send(c.b, &normal);
    assert(freed == 3);
    callback(context);
    assert(freed == 3); // Superseded buffer freed exactly once.
    SwitchGate other(ops);
    Ccb d;
    unsigned before = sent;
    other.send(d.b, &normal);
    other.send(d.b, &close);
    other.send(d.b, &normal);
    assert(sent == before + 3); // Ordinary codecs unchanged.
    SwitchGate failure(ops);
    Ccb e;
    fail_alarm = true;
    before = sent;
    failure.send(e.b, &close);
    failure.send(e.b, &ll);
    assert(sent == before + 2);
    fail_alarm = false;
    // Captured BT-B10 sequence: ordinary aptX -> Close -> genuine HD.
    // SetConfiguration must not be sent immediately after Close, while the
    // receiver may still be tearing down the previous media L2CAP channel.
    Message hd(3);
    const uint8_t hd_wire[] = {40, 16,   1, 0,    7, 13, 0, 255, 0xd7, 0, 0,
                               0,  0x24, 0, 0x12, 0, 0,  0, 0,   8,    0};
    hd.len = sizeof hd_wire;
    for (unsigned i = 0; i < sizeof hd_wire; i++)
        hd.data[i] = hd_wire[i];
    SwitchGate hd_gate(ops);
    Ccb h;
    before = sent;
    unsigned previous_schedules = schedules;
    hd_gate.send(h.b, &normal);
    hd_gate.send(h.b, &close);
    hd_gate.send(h.b, &hd);
    assert(sent == before + 2 && schedules == previous_schedules + 1 && last_delay == 500);
    Message hd_response = hd;
    hd_response.type = 3; // A peer rejection must pass through without waiting.
    hd_gate.send(h.b, &hd_response);
    assert(sent == before + 3);
    callback(context);
    assert(sent == before + 4); // Held HD is sent once after the existing guard.
    callback(context);
    assert(sent == before + 4);

    // A fresh HD connection and same-SEP Reconfigure do not acquire a delay.
    SwitchGate fresh_hd(ops);
    Ccb f;
    before = sent;
    previous_schedules = schedules;
    fresh_hd.send(f.b, &hd);
    Message reconfigure = hd;
    reconfigure.event = 5;
    fresh_hd.send(f.b, &close);
    fresh_hd.send(f.b, &reconfigure);
    assert(sent == before + 3 && schedules == previous_schedules);

    // Non-HD wire codecs, private Adaptive proxy, and malformed codec lengths
    // are not reclassified as real HD after Close.
    for (unsigned variant = 0; variant < 5; variant++) {
        SwitchGate bypass(ops);
        Ccb b;
        Message other_codec = hd;
        if (variant == 0)
            other_codec.data[12] = 1; // Different vendor codec ID.
        if (variant == 1) {
            other_codec.data[15] = 0xa5;
            other_codec.data[16] = 0xad; // Private Adaptive proxy marker.
        }
        if (variant == 2)
            other_codec.data[8] = 0x0a; // Different vendor.
        if (variant == 3)
            other_codec.data[5] = 42; // Truncated Adaptive-shaped media codec.
        if (variant == 4)
            other_codec.data[5] = 12; // Wrong HD codec envelope length.
        before = sent;
        previous_schedules = schedules;
        bypass.send(b.b, &close);
        bypass.send(b.b, &other_codec);
        assert(sent == before + 2 && schedules == previous_schedules);
    }

    // Reuse the existing ownership guards for delayed HD: disconnect and a
    // changed pending command cancel rather than sending to a stale session.
    for (unsigned variant = 0; variant < 2; variant++) {
        SwitchGate cancelled(ops);
        Ccb b;
        before = sent;
        unsigned previous_freed = freed;
        cancelled.send(b.b, &close);
        cancelled.send(b.b, &hd);
        if (variant == 0)
            b.b[0x15c0] = 0;
        else
            *(void **)(b.b + 0x15a8) = (void *)456;
        callback(context);
        assert(sent == before + 1 && freed == previous_freed + 1);
        callback(context);
        assert(freed == previous_freed + 1);
    }
    SwitchGate hd_superseded(ops);
    Ccb replaced;
    before = sent;
    unsigned previous_freed = freed;
    hd_superseded.send(replaced.b, &close);
    hd_superseded.send(replaced.b, &hd);
    hd_superseded.send(replaced.b, &normal);
    assert(sent == before + 2 && freed == previous_freed + 1);
    callback(context);
    assert(sent == before + 2 && freed == previous_freed + 1);
    puts("LL switch: deferred enter/exit, live responses, disconnect/replacement cancellation, "
         "real HD Close guard, proxy/non-HD bypass, normal path and allocation fallback passed");
}
