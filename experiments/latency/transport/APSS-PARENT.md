# APSS parent behavior for the existing ADSP container

An APSS program can supply the parent protocol through the existing HAL GPR
instance without constructing a new DSP parent container. This conclusion is
limited to the reviewed local ADSP container and untracked allowlisted metadata.
It does not establish audio performance or authorize standalone GPR access.

## Why the parent property is accepted locally

The target stock container `0xe00000e6` has process-domain property
`0x08001014=2` and already contains parent property `0x080010cb=0xffffffff`.
The fresh graph transform replaces that four-byte parent value in place with
the registered APSS client port `0x535a6101`; its property count and size stay
unchanged. The neighboring container's property stays unchanged.

Actual APM code:

1. `0xb0245834..74` locates `0x08001014` and loads the container's process domain.
2. `0xb024588c` calls the GPR local-domain query.
3. `0xb0245894..98` compares the two domains. Equal domains return at
   `0xb024589c` without performing a parent-node lookup.
4. The lookup at `0xb0245918` is only in the different-domain branch. A local
   ADSP container does not need an ADSP node named `0x535a6101`.

Actual container property dispatch at `0xb0250748..54` calls `0xb025210c` for
`0x080010cb`. A nonzero, non-`0xffffffff` parent value creates the eight-byte
offload-info object at `GC+0x170`, sets its satellite flag and stores the value
as client ID. RD at `0xb026178c` and WR at `0xb0263030` check that pointer before
dispatching OLC client configuration. The property must be part of normal
fresh OPEN, before the endpoint parameters are applied.

The matching public source is
[apm_check_and_cache_satellite_container_config](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/apm/ext/offload/src/apm_offload_utils.c#L375)
and [cu_create_offload_info](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/ext/offload/src/cu_offload_util.c#L312).
The actual address evidence uses the firmware identity in
[TRANSPORT-PROOF.md](TRANSPORT-PROOF.md).

## Why registered events can reach APSS

The actual module-event sender at `0xb0251eac` loads the registered client's
source/destination domain bytes at `0xb0251f20..2c`, its port at `0xb0251f3c`,
token at `0xb0251f44`, and uses the requested event ID as opcode. It does not
force destination domain ADSP. An APSS-domain registration therefore supplies
the return path. The public implementation is
[cu_raise_module_events_to_clients](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/ext/offload/src/cu_offload_util.c#L20).

RD/WR client configuration is `{client_id=1, gpr_port_id=0x535a6101}`. The
container parent ID and endpoint client port agree. WR reconstructs a tracked
metadata destination using that parent port and the submitting packet's
source domain. The current transform forbids tracked metadata, token-bearing
records and header extensions, so it must not invent tracking acknowledgements
or accept internal pointer-bearing metadata. EOS/DFG causes a request for the
normal owner to drain/close the fresh graph.

The public RD close path explicitly destroys `cached_md_list_ptr` with
`is_dropped=true` before destroying the external output. Thus ordinary owned
graph CLOSE is the appropriate endpoint-lifetime boundary for a rejected
tracked record; silently discarding that record and continuing playback is
not. The parent must keep its callback registered until the close and memory
cleanup complete. See
[gen_cntr_cmd_handler.c](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/core/src/gen_cntr_cmd_handler.c#L1742).

## Parent duties implemented by the native transport

Each event is registered directly with its owning endpoint through
`APM_CMD_REGISTER_MODULE_EVENTS=0x0100100e`. The registration token remains the
event's expected token, so an event arriving before its own basic registration
ACK must be handled as an event.

| Endpoint | Event | Parent action |
| --- | --- | --- |
| RD | `0x06001000` and `0x03001009` media format | Require raw-compressed format and exact bounded payload, forward it to WR with `0x04001001`, wait for its basic ACK. A changed format ends the epoch. |
| RD | `0x03001004` operating frame size | Require type RD, exact RD MIID and positive size within the recorded data capacity. |
| RD | `0x03001006` upstream peer properties | Require only type1 upstream RT with boolean values; forward to WR through `0x080011c3`. |
| WR | `0x03001005` downstream peer properties | Require only type2 downstream RT or type3 topology state; forward to RD through `0x080011c3`. |
| RD | `0x03001007` upstream state | Require one type3 topology-state property and observe it without forwarding it to WR. |
| RD | `0x06001001` EOS/discontinuity, `0x06001003` data-path property | End the epoch and request ordinary owned graph cleanup; do not guess metadata tracking or drop/EOS semantics. |

The peer-property parameter is a `uint32 count` followed by eight-byte
`{property_type,property_value}` pairs. Types 1/2 are upstream/downstream RT,
type3 is topology state. Boolean values must be0/1 and topology state0..3.
Validate the complete list against its event direction before changing state
or queuing any parameter. Type4 data-flow/EOS, type5 and every unknown or
misdirected type end the epoch. Duplicate forwarded properties are suppressed.
The parameter payload is padded to eight bytes in
the in-band APM request while its declared parameter size excludes padding.

After the ordinary owner reports its successful PREPARE and START, the parent
seeds only RD's downstream topology state to STARTED (`count=1,type=3,value=2`).
The seed ACK is bit1 of `initial_peer_acks`; no bit2/WR state seed exists.
After that ACK, every actually observed queued peer update has been ACKed,
a bounded nonzero RD operating frame size, acknowledged
mappings/event registrations and accepted
actual codec INIT, it posts at most two owned RD buffers. This bootstrap must
precede media-format acceptance: the primary OLC OPFS handler explicitly notes
that the output media-format event does not arrive until read buffers are sent,
then calls `sgm_send_all_read_buffers`. See
[spdm_port_event_handler.c:21..55](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_port_event_handler.c#L21).

The additional RD state event `0x03001007` is observational rather than a
mandatory bootstrap ACK. STARTED is2 and topology property type is3. A repeated
STARTED setting can succeed without an event: actual firmware
`0xb024f144..0xb024f14c` skips the callback for PREPARED or an unchanged state;
only the changed-state path reaches callback `0xb024f190`. This matches
[cu_ext_port_util.c:318..331](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/core/src/cu_ext_port_util.c#L318).
The sender `0xb0251dc4..0xb0251e40` builds the16B `{ep_miid,1,3,state}` payload.
The normal owner's START ACK establishes the receipt-queue cutoff: queued older
PREPARE/STOP events may be superseded, while a newly received non-STARTED state
revokes the epoch even before ACTIVE. Peer ACKs and the owner's START remain
required; no state is inferred from a setting or diagnostic field.

The directional restriction is a memory-layout requirement, not an optional
optimization. The real OLC selects WR only for upstream-RT type1 and RD for
downstream-RT type2 or topology-state type3; its separate RD upstream-state
handler does not forward that event to WR. See
[spdm_state_prop_handler.c:290](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_state_prop_handler.c#L290)
and [its upstream-state handler](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_state_prop_handler.c#L334).
The actual WR property dispatch at `0xb026066c..0xb026067c` passes its external
input handle to common handler `0xb026003c`. For type3, `0xb024f0c8..0xb024f0cc`
adds the external-output CU offset and stores the state at offset+4. The GC
constructor stores both CU offsets as0x24 at `0xb0274250..0xb0274280`; applying
type3 to WR therefore overwrites its external-input `cu.bit_mask` at+0x28.
The path wait-mask code reads that same field at `0xb027baa8..0xb027bac8`.
A successful basic parameter ACK does not make this wrong-direction cast safe.
This defect was found after the tenth trial submitted two validated WR buffers
without receiving completion; the overwritten field explains a queue-scheduling
failure, while the complete causal scope of the subsequent DSP non-response
has not been measured.

No WR RT value is fabricated. A fresh external input is allocated zeroed by
[graph_utils.c:1515](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/graph_utils/src/graph_utils.c#L1515).
Its normal WR/OLC client setup installs the input vtable and default single
buffering without assigning an upstream-RT value. Retain that initialized
state until an actual type1 property arrives, then await its ACK. The common
forward-state helper explicitly does not communicate STARTED downstream:
[cu_ext_port_util.c:154](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/core/src/cu_ext_port_util.c#L154).

Completed reads remain parked in submission order until the actual raw
media-format event is accepted by WR and every data gate is positive. The
activation deadline still bounds this wait. Strict validation of the first
actual R3 buffer precedes any positive TTP change or WR buffer submission.
STOP/fault/CLOSE retain their existing buffer-ownership and cleanup rules.
An unreviewed/rejected INIT closes the candidate graph; delta zero does not
authorize an unknown codec parser.

The fixed-format WR side permits one outstanding data command. This follows
the reviewed parent's steady buffer-pool policy: both branches of
[spdm_update_num_ipc_buffers](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_data_buf_handler.c#L749)
request one buffer. The sole WR send caller marks that node in use, and the
[pool exhaustion path](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_data_handler.c#L844)
waits for WR_DONE before accepting more input. It is a host credit policy,
not a claimed hardware or signed-DSP queue limit; format resizing can retain
deprecated pool nodes temporarily. Two owned RD buffers may still be prefetched,
but their exact completed returns stay parked in stream order until the single
WR credit is returned by a matching, address-bearing successful WR_DONE.
Unknown tokens, BASIC responses and invalid completions never grant credit.
Pause/fault/CLOSE can retire parked reads without forwarding them. This credit
limit does not itself alter flags, framing checks, metadata, TTP arithmetic or
cleanup bounds. Continuous real-device progress still requires measurement.

WR wire flags follow the actual OLC translation separately from the pure parser
result. The normal
[OLC sender at spdm_data_handler.c:612](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/olc/driver/data_process_mgmt/src/spdm_data_handler.c#L612)
sets only timestamp-valid/continuation bits31/29; it never forces EOF bit4.
The reviewed RD per-frame metadata sets only timestamp validity in its flags:
[gen_cntr_rd_sh_mem_ep.c:1105](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/rd_sh_mem_ep/src/gen_cntr_rd_sh_mem_ep.c#L1105).
A bounded RD output, including a C0 supplemental wrapper, therefore does not
establish codec EOF or timestamp continuation. After the unchanged strict result
validation, `shizuka_transport_wr_wire_flags()` copies only timestamp-valid bit31
into the outgoing WR command. Invalid parser flags still reject the buffer
before that translation. Timestamp values and payload/metadata bytes are unchanged.

This distinction is observable in the signed receiver: WR at
`0xb02636e8..0xb02636fc` extracts incoming WR flags bit4 and stores it in the
external-input EOF flag. The matching
[generic-container path](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/core/src/gen_cntr_data_handler_island.c#L1414)
propagates it into CAPI `end_of_frame`; it is not an ignored annotation.
The pure parser's legacy0x10 result remains internal and is not the wire value.
This change removes an unsupported assertion; sustained real-device progress,
especially across C0 supplements, remains a separate acceptance test.

The actual firmware's OLC output vtable at `0xb07923e4` agrees with this source:
its recreate callback `0xb025fc80..0xb025fc8c` stores the requested output size
at external-output `+0x38` and calls OPFS sender `0xb025fe68`. Its command-path
format callback `0xb025fca8..0xb025fcb8` calls formatter `0xb0262470` with
opcode `0x03001009`; output-write callback `0xb025fc04..0xb025fc28` sends
`0x06001000` when the format-changed flag is set. RD setup
`0xb0261940..0xb02619bc` pops its output queue and checks command `0x0400100b`.
The public matching callbacks are
[gen_cntr_offload_utils.c:1144](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/offload/src/gen_cntr_offload_utils.c#L1144)
and
[gen_cntr_offload_utils.c:1232](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/offload/src/gen_cntr_offload_utils.c#L1232).

The matching contracts are
[offload_sp_api.h](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/interfaces/module/offload/api/offload_sp_api.h),
[gen_cntr_offload_utils.c](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/offload/src/gen_cntr_offload_utils.c),
and [topo_utils.h](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/topologies/topo_utils/inc/topo_utils.h#L216).

## Two islands in one generic container

The actual firmware contains the parallel-path implementation:

| Firmware VA | Observation |
| --- | --- |
| `0xb0253aa0..0xb0253b9c` | Resets `gu+9` path count, marks module path index at `module+0x1a` invalid, then assigns a distinct index to each connected component. |
| `0xb0253bb0..0xb0253c5c` | Recursively propagates one component's index through its input/output connections and attached modules. |
| `0xb0276250..0xb02762b4` | Calls path assignment on `me+0x198`, stores at least one path, allocates/zeros a `path_count * 4` wait-mask array at `me+0x298`. |
| `0xb027ba74..0xb027bb10` | Accumulates each external input/output wait bit into the array entry selected by that port's module path index. |
| `0xb027bc2c..0xb027bc54` | Returns true if any path's wait-mask word is zero. |
| `0xb027b438..0xb027b46c` | Iterates each path and calls processing function `0xb0597a80` only for a zero-wait-mask path. An unready different path does not prevent the call. |
| `0xb027b47c..0xb027b4a8` | A second processing variant uses the same independent per-path condition with `0xb059a018`. |

This matches public `gu_update_parallel_paths()`, the generic-container wait
mask allocation, any-path readiness and per-path process loop. Therefore a
missing WR input does not inherently require blocking the encoder→RD island
in this firmware. It resolves the specific proposed all-input circular wait;
it does not measure processing budget, inherited port state or audio pacing.

References:
[graph path assignment](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/graph_utils/src/graph_utils.c#L2581),
[any-path readiness](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/core/src/gen_cntr_trigger_policy_island.c#L215),
[per-path process loop](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/core/src/gen_cntr_data_handler_island.c#L779).

The runtime trial must prove all prerequisite events, then one ordered RD→WR
completion with bounded waits. A stall, missing
property, unexpected frame size or metadata record requests normal fresh
graph cleanup. Do not alter the signed DSP or force graph/data sends past a
failed handshake. OLC also changes default single-buffering on existing
external ports, so actual pacing and receiver behavior remain measurements.

## Default-disabled RAW-island PREPARE barrier

The `SHIZUKA_RELAY_RAW_ISLAND=1` experiment can queue its two owned READ
leases after the actual owner's successful INIT/PREPARE and before owner START.
This changes the startup order only when the caller explicitly invokes
`shizuka_transport_prepare_reads(ctx, epoch)`. Default builds reject that API;
ordinary startup, pause/resume, strict R3 validation and WR credit policy remain
unchanged.

This is legal queueing, not an invented START state. The public
[CU GPR callback](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/core/src/cu_gpr_if.c#L24)
puts DATA commands onto the registered port's data queue without checking SG
START. In the reviewed firmware, callback `0xb02508b8..0xb02509d0` validates the
handle/queue and sends DATA at `0xb0250964` to queue-push `0xb002d11c`.
RD registration is at `0xb0261f90`; the output queue's capacity is128 at
`0xb02587c4`. RD setup `0xb0261940` pops the queue at `0xb026195c` and checks
READ opcode `0x0400100b` at `0xb02619a8`, followed by MAP, size and alignment
checks. These paths do not require STARTED. The normal OLC OPFS handler linked
above posts READs without waiting for output MF. The actual firmware identity
is SHA256 `de7ec2ab21308fbce5f5c87c845fd2995e8367f3a39978c6bde63573969dd3b6`.

After `prepare_reads` records the request, only the worker sends commands.
It first sets RD downstream state to truthful PREPARED1 and requires that exact
ACK, accepted INIT/PREPARE, mapped buffers, event registrations, actual nonzero
OPFS and all observed peer updates' ACKs. It then submits two READs without
waiting for MF. `shizuka_transport_prepared_reads_ready(ctx, epoch)` returns1
only while both current leases are at RD and the callback queue is drained;
0 means pending and a negative value is an invalid scope or existing failure.
The caller must release the transport executor lock while waiting and send
owner START only after1. The existing activation deadline bounds the wait.

The worker latches completion of that queue barrier because a real frame may
return after START is sent but before its synchronous ACK reaches the caller.
Such a frame stays parked in stream order. Only the real owner START ACK,
STARTED2 peer ACK, actual WR-accepted MF and unchanged strict R3 validation can
activate WR forwarding. No pre-START transform, positive counter or APPLIED
claim is produced. STOP/CLOSE and timeouts retain ordinary lease quarantine
and cleanup requirements. Internal diagnostic `startup_flags` bits12/13/14
mean request/PREPARED-seed-ACK/completed-queue-barrier; they are not authority.

A narrow startup empty-return budget is also explicit. The public
[RD MF-change path](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/rd_sh_mem_ep/src/gen_cntr_rd_sh_mem_ep.c#L487)
can release a READ while notifying MF, corroborated at firmware
`0xb0261bec..0xb0261c54`. The
[completion serializer](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/gen_cntr/ext/rd_sh_mem_ep/src/gen_cntr_rd_sh_mem_ep.c#L697)
and firmware `0xb0262078..0xb02620e4` copy the timestamp value even when invalid;
zero timestamp is therefore not a wire requirement. The experiment recycles
at most two exact owned EOK zero-data/zero-MD, zero-frame/zero-flag completions
after actual MF observation and before the first encoded frame, without
transforming, forwarding or counting them. This bound is a diagnostic policy,
not a DSP protocol maximum. Current `{metadata_control_flags=1,frames=1}`
disables MF metadata, and its4096B MD capacity exceeds the observed160B shape;
an MF-size retry is not expected in this fresh configuration. Metadata-only
returns are not discarded or treated as encoded data: they remain owned and
fail the unchanged encoded-data validation after START, or the bounded wait.
