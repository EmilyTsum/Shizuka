# Embedded GSL relay adapter contract

`relay_graph.c`, `relay_transaction.c`, and `relay_calibration.c` are bounded,
freestanding C components. They allocate nothing, have no mutable globals,
import no runtime functions, and never open an FD, call GPR, or issue a graph
command. They are not connected to the earlier passthrough payload. The owner
must supply checked views, per-graph state, locking, workspace, transport and
lifecycle completion. Building this component does not activate a route.

## Existing fresh node reuse

The plan replaces **existing, fresh-owned** SG `b00000ab` / GC `e00000e6` logger
instances; no new DSP instance ID is introduced:

| Existing node | Replacement |
| --- | --- |
| `43f7`, MID `0700101a`, PCM logger | RD endpoint MID `07001001`, 1 input /0 output |
| `43fa`, MID `0700101a`, encoded logger | WR endpoint MID `07001000`, 0 input /1 output |

The PCM chain `43f8 -> 43f7 -> 43fb` becomes `43f8 -> 43fb`. The encoded
`43fb -> 43fa` edge becomes `43fb -> RD43f7`. WR43fa retains its original
external output `43fa:1 -> connproxy4098:2`, including when that link is supplied
separately from this SG's ACDB data. Other edges and all IMCL parameters are
preserved byte for byte.

Both logger IDs occur only in SG00ab/GCe6 across all1042 unique saved stock graph
blobs, in36 of those blobs. No individual logger calibration parameter occurs
in those GRAPH_OPEN bodies. This is inventory evidence for exact node reuse,
not proof of fresh runtime ownership by itself. The owner additionally checks
the exact GSL build, selected Adaptive GKV and SG open/start/stop refs1/0/0 in
the original GSL open/close lock. A shared or already-running SG bypasses before
any size change. `NAMESPACE_OWNED` means that existing-node replacement scope;
no standalone missing-port query or new ID reservation is involved.

The existing e6 property `080010cb` must appear exactly once with value
`ffffffff`; its value alone becomes the owned APSS callback/parent port. e5 and
every other container property remain intact. RD `0800114d` and WR `080010c0`
receive `{client_id=1, owned_port}`. RD `08001007` receives `{1 frame, flags1}`.
Three24-byte parameters add72 bytes; one removed connection saves16. The fixed
GRAPH_OPEN body growth is therefore **56 bytes**. Transport-ready is a required
owner fact: merely creating the parent property does not establish that the
actual OLC control protocol is serviced.

## Extended hook and graph transaction

The PLT bridge retains the public first five ACDB arguments and invokes the
original function once. For the exact build SHA
`4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc`, metadata is:

| Operation | Opcode | Command/response sizes | Return RVAs | Graph register |
| --- | --- | --- | --- | --- |
| SPF size /driver /fill | `acdb0002` | 24 /28 | `246e4`, `247d0`, `24e20` | x28 |
| Nonpersistent cal size /fill | `acdb0004` | 36 /12 | `1e1c0`, `1e2d8` | **x21** |

The original passthrough gateway always captures x28. A runtime gateway must
select x21 for the exact calibration operation/call sites. In that routine,
x20 is the selected GKV-node pointer; x28 is **SG-object array**, and w26 is
**SG count**, not the GKV keys/count. x6 remains original LR and x7 remains the
resolved original ACDB function. The runtime's ninth argument, at entry SP+0,
carries raw x20; the tenth carries the original x29 frame pointer, captured
before the gateway installs its own frame. Keep SP16-byte aligned and
preserve x19..x29. Do not dereference x20 in the assembly gateway or for unknown
call sites. All other callers keep passthrough behavior.

The stock instructions establish these node offsets independently of current
public struct layouts: `1de7c` loads GKV count at node+`18`, `1de8c` loads keys
at node+`20`; `1def0` loads SG count at node+`38`, and `1df1c..1df24`
dereferences SG-object pointers at node+`40` to read each SG ID. The helper
receives this node in x2 and saves it to x20 at `1e130`; x21 receives the graph
at `1e154`. Both remain unchanged through the calls returning to `1e1c0` and
`1e2d8`. Only after exact CAL-site, successful original call, owned graph and
epoch checks may C read node+`18`/`20` as the current GKV count/pointer.
No `dlopen`/`dlsym` import or PLT entry exists in this exact GSL, despite its
libdl dependency; the embedded bootstrap supplies its existing-GOT table.

During the fresh graph's default calibration, node+18/+20 are still zero:
stock writes the GKV into that node only after the open helper succeeds.
`runtime/default_cal_context.c` recognizes only this exact original stack
chain: CAL caller251f0, parent caller23e80, adjacent0x190-byte frames,
the original node at parentFP-e0 and actual GKV-vector pointer at parentFP-d8.
It also checks command/response stack addresses, accepted OPEN ownership,
fresh refs, thread, source registration, complete SG set and every original
GKV pair against the frozen OPEN. It returns that actual original vector;
it does not substitute the cached OPEN keys. Explicit set_cal and graph-change
callers cannot use this fallback. An XPACLRI leaf strips only a copied metadata
LR for comparison and preserves its own real return address. It never branches
to a stripped value or changes a saved caller LR.

Use byte-safe loads for the packed ABI, never native aligned struct casts:

- Graph command24B: SG count0, SG-array pointer4, GKV count12, pointer16.
- Graph response28B: driver count0/size4/pointer8, SPF size16/pointer20.
- Calibration command36B: SG count0/pointer4, prior CKV count12/pointer16,
  new CKV count24/pointer28.
- Calibration response12B: blob size0, blob pointer4.

After a successful known graph ACDB call, call `shizuka_relay_after_acdb` with
the checked arrays and scope. A positive, canonical provider value1..150 ms is
snapshotted on the first size query. Its revision and value remain immutable
through the driver/fill stages and the whole graph epoch. A later setting
change does not change an in-flight graph's delay.

ADVERTISE means update SPF response size at offset16. COMMITTED means the
validated56-byte-larger body has been copied from separate workspace into the
expanded ACDB allocation; update that same response size. ABORT means fail this
fresh OPEN and let GSL unwind. Never return a shorter successful blob after
advertising growth. Unknown caller/library/GKV, disabled/zero configuration,
unowned IDs, incomplete transport, or non-fresh refs bypass before growth.
Workspace and allocation capacities are actual owner-known capacities, not
just the size ACDB last returned.

Only a matched GRAPH_OPEN basic ACK may call `shizuka_relay_open_complete`.
The synchronous basic-wait wrapper records ACK evidence before the wake, then
publishes the accepted graph state after the original basic wait returns and
**before its caller resumes** default calibration. Queuing state publication
to a later worker creates a race where logger calibration could escape
filtering. Host locking must serialize all state access; these C functions
contain no locks or atomics. The APM callback may run on a different thread,
but graph/epoch/source/parent identity must match.

### Pre-wake control ACK evidence

The exact stock `gsl_signal_set@PLT` is `39488`, its final branch is `39494`,
and its GOT entry is `3c560`. The original entry `388f0` branches to `2c45c`.
Its ABI is `(signal=x0, event_flags=w1, status=w2, packet=x3) -> w0`.
A tail gateway can add the resolved original function from x17 as x4 without
changing these arguments. This direct PLT detour needs no callback replacement
or additional CFI target. The graph callback at `23c38..23c48` supplies status
from the basic payload, flags1, the graph signal and the original packet.

Record full response evidence **before** calling the original signal function:
it stores the packet at `2c508` and wakes the waiter at `2c50c`. Do not hold
the wait-table lock while calling it, because it may also free an older packet.
The response receiver's TID is not the synchronous waiter's TID. Cache the
waiter's exact signal and scope when entering the basic wait, then cache its
full outgoing token at async-send entry after GSL inserts debug bits.

`runtime/src/control_ack.c` checks exact32-byte basic response framing, opcode,
full token, reversed ports/domains, flags1 and payload/argument status equality.
It records a matching NACK as a nonzero status; the wait wrapper must reject it.
Its bounded observer requires exactly one active wait sharing the signal and
invalidates same-signal malformed, unexpected or duplicate events. A later
wrong-token event must invalidate an earlier good ACK because stock can replace
the signal's stored packet. Final acceptance also requires the original basic
wait to return zero, exactly one recorded ACK with status zero, and the same
owned epoch. No packet pointer is retained after the observer returns.

The public [GSL common source](https://github.com/Audioreach/audioreach-graphservices/blob/558ddfc2c87019496957855c209dd5ece4f6aceb/gsl/src/gsl_common.c)
removes debug bits through the outgoing packet pointer. Stock does the same
at `2c8e4..2c8f0`. The allocator may recycle that outgoing packet as its
response, so observing only at `__gpr_cmd_free` after the wait can see a
normalized response token. A lower12-bit comparison is insufficient.
Furthermore, stock `gsl_signal_set` lacks the current public source's expected
token check; a zero original wait status alone does not attest the full token.
Stock basic-wait body `2cc90..2cd8c` frees the response at `2cd60` on the waiting
thread before returning. The pre-wake copy avoids both token-alias and
post-return-publication races.

After OPEN ACK, the effective delta remains zero until the same epoch has an
accepted encoder INIT and independently confirmed MID07001082, non-Lossless
HQ4096,44100Hz and wrapper version3. HQ INIT alone cannot identify Lossless.
The active-delta function exposes only the frozen provider value for that
reviewed profile. Zero is not permission to run a profile-specific parser on
unknown/LL data. The transport must establish reviewed forwarding or abort and
restore before using an unsupported negotiated profile. Close/reset immediately
revokes the profile and pending calibration state.

## Subsequent logger calibration

Saved ACDB calibration keys include `08001026`, `08001031`, and `08001313` for
both replaced logger IDs. The normal GSL nonpersist path must remove these
logger parameters so they are not delivered to the RD/WR framework modules.
`shizuka_relay_after_calibration` requires an ACTIVE owned epoch, exact current
GKV, SG00ab membership, matching query/fill thread and identical SG/CKV vectors.
The calibration command contains CKVs **but no GKV**; the embedding owner must
obtain the current GKV from its checked graph/epoch context.

Size query capacity is left unchanged. Fill validates the complete aligned
16-byte APM parameter stream before compacting in place. Only the exact three
logger PIDs for43f7/43fa are removed. Every other parameter, its padding and
ordering is preserved. Write the returned actual_size at AcdbBlob offset0;
trailing allocation bytes are not part of the resulting payload. If everything
was removed, CAL_EMPTY means return `AR_ENOTEXIST=19`, which the original outer
GSL calibration path handles as a benign absence, rather than sending an empty
SET_CFG. Preserve an original nonzero ACDB status. Unknown or unowned calls
bypass; malformed owned data aborts without a partial compaction.

Actual code at `1e350` loads the post-fill blob size and `1e358` stores it in
the outgoing APM header's payload_size. Thus the compacted actual size is used,
while the original larger allocation can be safely freed by the normal owner.
Primary source is `gsl_graph.c:821..891` and ACDB API packed structures. Reset
the separate calibration transaction on owner close/abort/reset.

## Validation and initialization boundary

`python -B -m unittest -v test_relay_graph.py` compiles a host shared test library
in an external temporary directory and uses synthetic data only. Eight tests
cover exact graph bytes and external-edge/IMCL preservation, graph malformed
inputs/truncation/mutations, provider revision and ACK/epoch/profile gates,
capacity/overlap rejection, and calibration compaction/unchanged query capacity/
unknown scope/late mutation/empty handling. The C sources compile freestanding
for NDK ARM64; external imports are absent apart from calls between these
components. Existing graph/loader/device acceptance is a separate checkpoint.

The standalone `runtime/tests/control_ack_test.c` runs with host ASan/UBSan and
requires no device or private fixture. It covers every single-bit wire mutation
(except opaque GPR client_data), all32 token bits, lower12 normalization,
unaligned/bounded input, mismatched status/flags, NACKs, allocator alias,
duplicate and overwritten responses, and nonunique signals across16 waits.
`control_ack.c` also compiles freestanding for NDK ARM64. This tests response
attestation only; real GSL scheduling and receiver behavior remain runtime
acceptance checks.

The normal GSL initialization order in primary `gsl_main.c:872..` is logging,
GPR init, data logging, ACDB init, SG pools/lists/locks/signals, GSL main-port
registration, MDF setup and SPF readiness. Graph ACDB calls occur after this
normal owner setup. The injected code must never call GPR init/deinit or open a
second audio-pkt reader. It reuses the existing owner and callback registry.
