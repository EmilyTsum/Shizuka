# HAL-owned relay state machine

`hal_transport.c` is freestanding C11 with caller-owned POD storage and a
host function table. It does not import libc, STL, GPR, OSAL or a device API.
The Android ARM64 object has no undefined symbols. All vendor operations must
be bound by the root-reviewed bootstrap inside the already running HAL.

No second transport FD, `gpr_init`, `gpr_deinit`, library loader, global graph
close or graph-open API exists here. The actual driver's shared queue and
release behavior make those boundaries necessary. This implementation is
source-tested; it has not completed a real RD/WR transfer.

## Bootstrap contract and checkpoints

1. Allocate zero-filled, eight-byte-aligned, immovable storage of
   `shizuka_transport_context_bytes()`. Initialize with the exact ABI/structure
   sizes, HAL/DSP generation identities and unique nonzero token seed.
2. Supply wrappers around the HAL's existing GPR callback registry and direct
   `__gpr_cmd_alloc_ext`/`__gpr_cmd_async_send`/free functions. `send()` must copy
   the stack-backed normalized payload before returning. Do not use
   `gsl_send_spf_cmd`: it rewrites debug-token bits. A failed send returns 1
   only if it is positively known not to have been sent; any other failure is
   treated as uncertain and is never retried.
3. `attach()` registers owned APSS client port `0x535a6101`. It sends nothing.
   `step()` queries SPF state through the same existing reader. A successful
   READY response sets `registration_ready`. This proves routing/readiness,
   not DSP namespace ownership.
4. The provider may transform only its normal fresh stock Adaptive SG. The
   current graph transform reuses owned logger MIIDs RD=`0x43f7`, WR=`0x43fa`
   in SG `0xb00000ab`, container `0xe00000e6`. It replaces the existing parent
   value and strips logger-only calibration in that same transaction. This
   avoids claiming that an unsupported GET proved arbitrary new MIIDs unused.
5. After ordinary GSL OPEN acknowledgement, call `bind_graph()` with the exact
   graph epoch, generations, fixed delta and the checked ownership facts.
   Actual INIT attestation may still be zero. Only two private allocations,
   MAPs and endpoint event registrations are then allowed. Wait for
   `configuration_ready` before continuing normal codec configuration. INIT
   can otherwise precede registration and lose its first media-format event.
6. Once the actual codec INIT is accepted and independently identified as
   reviewed R3/HQ/44.1 kHz, call `attest_profile(epoch,1,1)`. A rejected or
   unknown current-epoch INIT requests ordinary graph close. There is no
   unknown-profile delta-zero pass-through. The delta is immutable for the
   graph epoch; a new UI value waits for a new session.
7. Notify successful ordinary PREPARE and START through `graph_stage()`. The
   worker completes [the APSS parent protocol](../APSS-PARENT.md). No data is
   sent until mappings/events, accepted INIT, format ACK, peer START ACKs,
   RD upstream-started and bounded operating-frame size are all present.
8. On removal, authority expiry, root-session loss, terminal close, an error or a changed
   HAL/graph identity, stop activation and notify STOPPING. `need_graph_close`
   asks the owner to use its normal fresh graph STOP/CLOSE path. This component
   cannot close somebody else's graph and never emits an APM close command.
   Only the owner's successful CLOSE acknowledgement or an independently
   confirmed changed DSP generation discharges outstanding buffer leases.

An ordinary pause uses a different path from terminal `STOPPING`: notify
`SHIZUKA_GRAPH_PAUSING` before the normal GSL STOP command, then
`SHIZUKA_GRAPH_STOPPED` only after its successful ACK. Submissions stop while
the maps and event registrations remain owned. That ACK does not discharge a
buffer lease; the worker waits for every exact DONE before reporting
`pause_ready`/`SHIZUKA_TRANSPORT_PAUSED`. Empty successful RD flush returns are
accepted. Nonempty returned frames require the optional strict
`validate_discard` callback; it validates known framing/untracked metadata
without adding TTP, publishing a transform sample or sending WR. Unknown EOS
or tracked metadata still requests an ordinary close.

A later successful `STARTED` on this same epoch re-arms peer START and the
preserved media-format command, waits for their ACKs and a fresh bounded RD
operating-frame-size event, then resumes with the same accepted INIT and fixed
delta. The earlier active epoch already proved the RD state-event route; the
owner's new successful START ACK is the resume state evidence. Queued older
STOP/PREPARED properties are drained before rearming. A fast START intent can
wait for outstanding STOP returns rather than racing old buffer leases.
Mappings are not unmapped and slots are not freed on ordinary pause. Real
pause/resume has not yet been tested; a missing DONE or unsupported returned
metadata fails this contract instead of silently claiming a working pause.

All APIs except `receive()` belong to one worker. `receive()` is the sole
producer on the HAL's existing single GPR receiver thread. It only copies a
normalized packet into a bounded 64-record queue. The outer wrapper validates
the real GPR header, domains, packet length and pointer before calling it,
then consumes/frees its actual GPR packet once. Overflow ends the epoch; it
cannot silently lose a DONE packet and reuse its allocation. `notify()` runs
on the worker and must not reenter the worker APIs or synchronously stop HAL.
The complete post-START handshake has a control-timeout deadline even when
no individual command is pending; a missing unsolicited format/state/frame-size
event cannot leave the modified graph stalled indefinitely. Waiting for an
ordinary STOP ACK also has that bound, separately from each buffer's lease
timeout. Expired handshakes request owned close rather than forcing data.

`unregister_port()` must synchronize callbacks that were already executing;
calling the vendor deregistration function alone is not a proven callback
lifetime barrier. Keep callback code/context resident until detach succeeds.
Reinitializing an attached context is rejected.

## Memory and frame ownership

The allocator adapter uses the reviewed 80-byte OSAL ABI inside the stable
cookie, ADSP subsystem byte 2 and default flags. It returns a private 16-KiB
offset-mode allocation with CPU START ownership. Its FD and CPU span must be
distinct from the other slot's. Each slot has 4 KiB data at offset zero and
4 KiB metadata at offset 4096. Metadata and data share one ownership state;
different slots use separate dma-bufs.

The worker obtains each acknowledged APM map handle while retaining its FD.
CPU END precedes the first RD submission. Successful address/handle/token
matching RD_DONE allows CPU START, complete validation and the host transform.
CPU END precedes WR. A slot is never reused before both WR statuses succeed.
Returned reads are transformed and written in submission order even if their
callbacks arrive out of order. Timestamp bits, validity and EOF are checked.
Empty forwarded metadata is serialized with zero address, handle and size.

The host transform must validate all input before changing memory, preserve
encoded data length, bound metadata output and return exact WR flags and
timestamp. Its reviewed profile gate remains mandatory. The state machine
itself does not contain a codec parser; a synthetic callback in its test is
deliberately separate from the real transform implementation.

On CLOSE, each known handle is unmapped once. Vendor free follows only a
successful UNMAP ACK and known CPU START ownership. An unmap error/timeout,
unknown MAP outcome, cache-sync failure or callback loss leaves cleanup
uncertain. It blocks detach and never causes a guessed handle, retry or early
free. A late exact response may resolve its original operation. A genuinely
changed DSP generation can invalidate remote leases/maps, but it does not
magically repair unknown host cache/allocator ownership.

Status reports completed WR frames and rewritten wrappers separately from
the requested epoch delta. A WR completion proves accepted carrier transfer,
not receiver delay or increased radio/quality budget.

## Build and synthetic checks

```sh
cmake -S experiments/latency/transport/native -B /path/to/work/transport-build \
  -DSHIZUKA_TRANSPORT_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /path/to/work/transport-build
ctest --test-dir /path/to/work/transport-build --output-on-failure
```

The test uses an in-memory fake HAL/DSP, not real vendor libraries or captured
audio. It exercises OPEN-before-INIT, registration events before ACK, MAP
layout, format and peer handshakes, two-slot ownership/cache transitions,
out-of-order RD completion, no reuse before WR_DONE, malformed DONE,
transform/INIT rejection, queue overflow, send failure, MAP/lease timeout,
unmap error/no retry, acknowledged close/reset cleanup and ordinary
STOP/START reuse, including a fast resume and nonempty discarded pause frame.

For an ARM64 payload object use the NDK compiler with `-std=c11 -O2 -fPIC
-ffreestanding -fno-builtin -fno-stack-protector`, include `include/`, and
compile `src/hal_transport.c`. Run `llvm-nm -u` on the object: it must print no
undefined symbols. Production bootstrap linking must still check its callback
CFI types, relocation bounds, target build identity and the host adapter ABI.
