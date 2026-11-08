// SPDX-License-Identifier: GPL-2.0-only
#include "../src/aptx_ll_switch.h"
#include <assert.h>
#include <stdio.h>
using namespace s25::ll;
static unsigned sent, freed;
static void (*callback)(void *);
static void *context;
static bool fail_alarm;
static void *alarm() { return fail_alarm ? nullptr : (void *)1; }
static void schedule(void *, uint64_t ms, void (*cb)(void *), void *ctx) {
    assert(ms == 500 || ms == 20);
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
    puts("LL switch: deferred enter/exit, live responses, disconnect/replacement cancellation, "
         "normal path and allocation fallback passed");
}
