// SPDX-License-Identifier: GPL-2.0-only
#include "../src/protocol.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <array>
#include "../src/settings.h"
struct VscComplete {
    uint16_t opcode, length;
    uint8_t *bytes;
};
using Callback = void (*)(VscComplete *);
extern "C" void s25_vendor(uint16_t, uint8_t, uint8_t *, Callback);
extern "C" const s25::State *s25_debug_state();
extern "C" void s25_anchor() {}
extern "C" uint8_t tramp_prs_cfg(void *, uint8_t *, uint16_t, uint8_t *, uint8_t) { return 0; }
extern "C" void tramp_bld_cfg(uint8_t **, void *) {}
extern "C" bool tramp_to_hal(uint8_t *, void *) { return false; }
extern "C" bool tramp_enabled(const uint8_t *) { return false; }
extern "C" bool tramp_specific(void *, uint8_t *) { return false; }
struct Sent {
    uint16_t opcode;
    uint8_t size;
    std::array<uint8_t, 256> bytes{};
    Callback callback;
};
static std::vector<Sent> sent;
extern "C" void tramp_vendor(uint16_t op, uint8_t n, uint8_t *p, Callback cb) {
    Sent item{op, n, {}, cb};
    memcpy(item.bytes.data(), p, n);
    sent.push_back(item);
}
struct Reply { unsigned id; uint8_t status, sub; };
static std::vector<Reply> replies;
static void record(unsigned id, VscComplete *p) {
    assert(p->opcode == 0xfd5d && p->length == 2);
    replies.push_back({id, p->bytes[0], p->bytes[1]});
}
static void first(VscComplete *p) { record(1, p); }
static void second(VscComplete *p) { record(2, p); }
static void third(VscComplete *p) { record(3, p); }
static void cancel_from_start(VscComplete *p) {
    record(1, p);
    uint8_t cancel = 2;
    s25_vendor(0xfd5d, 1, &cancel, second);
}
static void complete(size_t index, uint8_t status) {
    auto cb = sent.at(index).callback;
    assert(cb);
    uint8_t bytes[2] = {status, 10};
    VscComplete event{sent.at(index).opcode, 2, bytes};
    cb(&event);
}
static void reply(unsigned id, uint8_t status, uint8_t sub) {
    assert(!replies.empty());
    const auto &r = replies.back();
    assert(r.id == id && r.status == status && r.sub == sub);
}
static uint8_t command[57] = {1, 0x40};
static uint8_t stop = 2;
static size_t start(Callback cb = first) {
    size_t index = sent.size();
    s25_vendor(0xfd5d, 57, command, cb);
    assert(sent.size() == index + 1 && sent.back().opcode == 0xfc0a && sent.back().size == 66);
    return index;
}
static size_t stop_now(Callback cb = second) {
    size_t index = sent.size();
    s25_vendor(0xfd5d, 1, &stop, cb);
    assert(sent.size() == index + 1 && sent.back().opcode == 0xfc0a &&
           sent.back().size == 2 && sent.back().bytes[0] == 5);
    return index;
}
static void passthrough_stop() {
    s25_vendor(0xfd5d, 1, &stop, second);
    assert(sent.back().opcode == 0xfd5d && sent.back().callback == second);
}
int main() {
    auto *s = const_cast<s25::State *>(s25_debug_state());
    s->selected_seen = 1;
    uint8_t cie[43] = {42, 0, 255, 0xd7, 0, 0, 0, 0xad, 0, 0x10, 8};
    cie[9] = s25::kWireRate;
    memcpy(s->selected, cie, 43);
    command[9] = 2;
    command[23] = 0x6f;
    command[24] = 3;
    s->encoder_mode = 4096;

    // Successful START updates the controller mode and preserves command bytes.
    auto pending = start();
    assert(sent.back().bytes[1] == 10 && sent.back().bytes[15] == 0x7b);
    complete(pending, 0);
    reply(1, 0, 1);
    const auto &mode = sent.back();
    assert(mode.size == 6 && mode.bytes[0] == 0x13 && mode.bytes[1] == 1 &&
           mode.bytes[2] == 5 && mode.bytes[3] == 2 && mode.bytes[4] == 0 &&
           mode.bytes[5] == 0x10 && !mode.callback);

    // Failed STOP retains ownership; repeated pending STOP cannot overwrite its callback.
    pending = stop_now();
    auto count = sent.size();
    s25_vendor(0xfd5d, 1, &stop, third);
    assert(sent.size() == count);
    reply(3, 0x0c, 2);
    complete(pending, 0x0c);
    reply(2, 0x0c, 2);
    pending = stop_now();
    complete(pending, 0);
    reply(2, 0, 2);
    passthrough_stop();

    // A pending START owns its callback/mode. Queue cancellation without sending native STOP.
    pending = start();
    count = sent.size();
    s->encoder_mode = 8192;
    s25_vendor(0xfd5d, 57, command, third);
    assert(sent.size() == count);
    reply(3, 0x0c, 1);
    s25_vendor(0xfd5d, 1, &stop, second);
    assert(sent.size() == count);
    auto delivered = replies.size();
    complete(pending, 0);
    assert(replies.size() == delivered + 1);
    reply(1, 0, 1);
    // Exactly one deferred STOP, no late HQ/LL mode command.
    assert(sent.size() == count + 1 && sent.back().size == 2 && sent.back().bytes[0] == 5);
    s25_vendor(0xfd5d, 57, command, third);
    reply(3, 0x0c, 1);
    complete(sent.size() - 1, 0);
    reply(2, 0, 2);
    passthrough_stop();

    // START failure without cancellation releases ownership and forwards the real status.
    pending = start();
    count = sent.size();
    complete(pending, 1);
    assert(sent.size() == count);
    reply(1, 1, 1);
    passthrough_stop();

    // Failed fresh START plus a rejected cancellation must not invent permanent ownership.
    pending = start();
    s25_vendor(0xfd5d, 1, &stop, second);
    complete(pending, 0x1f);
    reply(1, 0x1f, 1);
    complete(sent.size() - 1, 0x0c);
    reply(2, 0x0c, 2);
    passthrough_stop();

    // Later LL mode is taken from its own START, not the previously rejected request.
    pending = start();
    complete(pending, 0);
    assert(sent.back().size == 6 && sent.back().bytes[5] == 0x20);
    // A completed stream does not block a fresh START after a link loss/reconnect.
    // If that replacement fails, ownership of the prior stream is retained for STOP.
    pending = start();
    complete(pending, 0x0c);
    reply(1, 0x0c, 1);
    complete(stop_now(), 0x0c);
    complete(stop_now(), 0);

    // A synchronous stack callback may immediately cancel its just-completed START.
    pending = start(cancel_from_start);
    count = sent.size();
    complete(pending, 0);
    assert(sent.size() == count + 2 && sent.back().bytes[0] == 5);
    complete(sent.size() - 1, 0);
    reply(2, 0, 2);

    // Null clients still own pending commands and must release ownership on completion.
    pending = start(nullptr);
    s25_vendor(0xfd5d, 1, &stop, nullptr);
    delivered = replies.size();
    complete(pending, 0);
    complete(sent.size() - 1, 0);
    assert(replies.size() == delivered);
    passthrough_stop();

    // A failed QTI STOP followed by native START transfers ownership only on success.
    uint8_t ordinary[57] = {1, 4};
    pending = start();
    complete(pending, 0);
    complete(stop_now(), 0x0c);
    pending = sent.size();
    s25_vendor(0xfd5d, 57, ordinary, first);
    assert(sent.back().opcode == 0xfd5d && sent.back().callback != first);
    s25_vendor(0xfd5d, 1, &stop, second);
    count = sent.size();
    complete(pending, 0x0c); // Failed handover: cancellation remains QTI.
    reply(1, 0x0c, 10); // Native START completion is forwarded unchanged.
    assert(sent.size() == count + 1 && sent.back().opcode == 0xfc0a);
    complete(sent.size() - 1, 0x0c);
    pending = sent.size();
    s25_vendor(0xfd5d, 57, ordinary, first);
    s25_vendor(0xfd5d, 1, &stop, second);
    count = sent.size();
    complete(pending, 0); // Successful handover: cancellation must be native.
    reply(1, 0, 10);
    assert(sent.size() == count + 1 && sent.back().opcode == 0xfd5d &&
           sent.back().size == 1 && sent.back().bytes[0] == 2);
    complete(sent.size() - 1, 0);
    reply(2, 0, 2);
    passthrough_stop();

    // Idle ordinary aptX / LL-shaped starts and unrelated commands retain original routing.
    s25_vendor(0xfd5d, 57, ordinary, first);
    assert(sent.back().opcode == 0xfd5d && sent.back().size == 57 &&
           sent.back().callback == first && !memcmp(sent.back().bytes.data(), ordinary, 57));
    uint8_t other[2] = {8, 9};
    s25_vendor(0xfc17, 2, other, nullptr);
    assert(sent.back().opcode == 0xfc17 && sent.back().size == 2 &&
           !memcmp(sent.back().bytes.data(), other, 2) && !sent.back().callback);
    puts("vendor: delayed START/STOP, queued cancellation, real failure forwarding, failed-stop "
         "retry, overlap rejection without callback/mode overwrite, and ordinary pass-through");
}
