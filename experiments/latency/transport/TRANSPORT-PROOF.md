# Transport evidence and remaining prerequisites

The checked-in transport code only constructs or validates byte strings. It
does not initialize GPR, open a device, allocate/map memory, create a graph or
send endpoint commands. Live RD → APSS → WR operation remains unimplemented
and unvalidated. This document records the evidence needed to implement it
inside the existing HAL owner. See [the lifetime contract](../TRANSPORT-LIFECYCLE.md)
for the required state transitions.

## Actual driver behavior changes the integration boundary

The reviewed Samsung `audio_pkt_dlkm.ko` has SHA-256
`30e3c82b05d74239ac0f3261a30866b6e330368ba30236dd5be4c6188a168ab6`.
Its ELF relocations and AArch64 instructions establish:

| Actual function/offset | Observation |
| --- | --- |
| `audio_pkt_open`, text `0x50..0x58` | Loads one global `.bss+8` device pointer and stores it as every file's private data. |
| `audio_pkt_read`, text `0x1c8..0x204` | Reaches that same device and receive queue; there is no per-open client-port queue. |
| `audio_pkt_release`, text `0xc8..0xfc` | Locks and drains that shared receive queue. |
| `audio_pkt_release`, text `0x124` | Unconditionally calls imported `spf_core_apm_close_all` on the valid device-data path. |
| `audio_pkt_release`, text `0x128` | Immediately calls imported `msm_audio_ion_crash_handler` on that path. |

These are actual module observations, not behavior inferred from a public
driver with the same filename. The reviewed public
[AudioReach driver](https://github.com/Audioreach/audioreach-kernel/blob/6b1abf0c7fb0b8931575f4ef1d1f0926cdc5714f/ipc/audio-pkt.c)
also shares a queue, but does not contain those two release calls. The binary
itself is intentionally absent from this repository.

The earlier standalone GET probe was incorrectly classified as read-only by
considering its command opcodes alone. Driver open/read/release are part of
the operation. Closing this driver's descriptor can affect existing graphs
even after a GET-only command. Process exit, a timeout and `gpr_deinit()` do
not provide an isolated cleanup boundary.

A private 16-KiB OSAL allocation/pattern/free check succeeded without GPR.
A subsequent standalone MAP returned an accepted handle; its single UNMAP
request timed out. At the same time the HAL's existing GPR reader logged the
probe's client port `0x535a6201` as unregistered, followed by unsupported basic
response opcode `0x02001005`. This is strong evidence that the HAL consumed
the UNMAP acknowledgement. Its status was not captured, so remote unmapping
is unconfirmed. This was not a successful MAP/UNMAP round trip. No relay graph
or RD/WR request was sent. An observed HAL PID change predated MAP; these
records do not establish that MAP caused a HAL process restart.

Do not repeat these standalone probes, retry the old handle or create a
second reader. A future relay must use the HAL's already initialized GPR
instance, existing transport descriptor and dispatcher. It may register a
fresh, owned callback port with `__gpr_cmd_register()` and deregister only that
port after all its responses and callbacks have drained. It must not call
`gpr_init()`, `gpr_deinit()` or close the HAL transport. The callback's code and
state must remain resident while a response can arrive. These APIs are
exported by the reviewed vendor `libar-gpr.so`; successful attachment and
callback lifetime management have not been implemented here.

`__gpr_cmd_is_registered()` queries the current process's host callback
registry. A false result does not reserve a DSP MIID or prove the remote
namespace unused. An unsupported GET or timeout proves neither fact.

## OSAL ABI and the FD-to-handle conversion

The reviewed `liblx-osal.so` SHA-256 is
`1d81e09e7eb9b9442218127036acb255f56857cadd220ed85dbed532619586ef`.
Its ABI matches the older engine
[ar_osal_shmem.h](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/ar_osal/api/ar_osal_shmem.h),
not the newer `proc_info`-based header in graphservices. The ARM64 structure
is 80 bytes with these offsets:

| Field | Byte offset/type |
| --- | --- |
| `cache_type`, `buf_size` | `0: uint32`, `8: size_t` |
| `mem_type`, `index_type` | `16: uint32`, `20: uint32` |
| `ipa_lsw`, `ipa_msw` | `24: uint32`, `28: uint32` |
| `pa_lsw`, `pa_msw` | `32: uint32`, `36: uint32` |
| `vaddr`, `metadata` | `40: pointer`, `48: uint64` |
| `num_sys_id`, `sys_id` | `56: uint8`, `64: pointer to uint8[]` |
| `platform_info`, `flags` | `72: uint32`, `76: uint32` |

Actual allocation implementation `0xf288` validates the one-byte subsystem
IDs, selects a DMA heap, allocates/maps a dma-buf, issues DMA CPU START|RW and
registers the FD with `/dev/msm_audio_ion`. Its successful output has physical
memory type `0`, offset index type `1`, `ipa_lsw = process-local FD` and
`ipa_msw = 0`. The host virtual address is a separate output. Input
`cache_type = 0` does not establish an uncached allocation: actual heap choice
uses `vendor.audio.gsl.shmem.dmaheap.uncached`.

Actual free implementation `0xf818` performs the paired CPU END|RW when
needed, host unmap, audio-ion unregister, FD close and metadata free. Never
interpret this process-local FD as an APM handle, host pointer or physical
address. Keep it alive through acknowledged APM UNMAP and outstanding data
completion. A fresh in-HAL implementation must manage OSAL's existing global
initialization/ownership instead of copying a standalone init/deinit sequence.

The normal vendor GSL MAP path at `0x28f48..0x29110` converts that FD into:

| MAP field | Value/meaning |
| --- | --- |
| Opcode | `0x0100100c` |
| GPR `client_data` | Bit 0 set for the OSAL offset-index FD path |
| Body, 20 bytes LE | `uint16 pool=3`, `uint16 regions=1`, `uint32 property=4`, `uint32 fd`, `uint32 0`, `uint32 allocation_size` |
| Property bit 2 | Offset addressing; bit 0 remains physical-memory type, bit 1 SPF-uncached remains clear |
| Accepted response | `0x02001001`, exactly one nonzero `uint32` map handle |
| UNMAP | `0x0100100d`, exactly the acknowledged owned `uint32` handle; GPR `client_data=0` |
| UNMAP completion | Basic response `0x02001005`, payload `{original_opcode=0x0100100d, status}` |

The offline helper encodes this specific offset-mode body. It requires a
positive, 4-KiB-aligned uint32 allocation size. Endpoint data/metadata addresses
are offsets into that recorded allocation; offset zero is valid. They remain
64-bit until range checks and LE low/high serialization. An address or offset
must be 64-byte aligned and its entire span must fit the acknowledged mapping.
This does not authorize passing an arbitrary open FD to DSP.

## OLC requires a container property before endpoint client configuration

Appending only RD parameter `0x0800114d` and WR parameter `0x080010c0` to an
ordinary container is insufficient. The actual firmware checks its container
offload-info pointer at `GC+0x170` before calling either OLC handler:

| Actual firmware VA | Observation |
| --- | --- |
| `0xb026178c..0xb0261794` | RD rejects a null pointer, otherwise calls OLC parameter handler `0xb0260438`. |
| `0xb0263030..0xb0263038` | WR rejects a null pointer, otherwise calls OLC parameter handler `0xb02605c0`. |
| `0xb0250748..0xb0250754` | Container-property dispatch recognizes `0x080010cb` and calls `0xb025210c`. |
| `0xb0252114..0xb0252118` | That function requires at least four payload bytes. |
| `0xb025214c..0xb0252154` | Parent ID zero and `0xffffffff` are ignored rather than creating offload state. |
| `0xb0252168..0xb025219c` | Allocates eight-byte offload info if absent, stores it at `GC+0x170`, sets satellite flag and stores the parent ID as client ID. |

This matches public
[cu_create_offload_info](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/spf/containers/cmn/container_utils/ext/offload/src/cu_offload_util.c#L312)
and [APM_CONTAINER_PROP_ID_PARENT_CONTAINER_ID](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/apm/apm_container_api.h#L311).
It statically identifies the missing property: `0x080010cb`, payload size four,
payload an owned, nonzero parent/client ID. It does not prove that a chosen
APSS parent ID works as a complete offload parent.

The public local-domain APM parser logs this property without looking up a
parent node. Its separate remote-domain satellite-cache path requires an
existing parent node. The actual satellite helper likewise performs a parent
lookup at `0xb0245918`; its surrounding domain semantics still need review
before relying on an APSS-only parent. Do not use a fake arbitrary ID as a
substitute for implementing the parent's event/state protocol.

Setting OLC client configuration also changes single-buffering policy across
the container's existing external ports. Effects on ordinary A2DP pacing,
upstream/downstream state propagation, offload property events and teardown
remain unvalidated. The current target is a fresh, normal GSL-owned Adaptive
SG `0xb00000ab`, container `0xe00000e6`, with only edge
`0x43fb:1 → 0x43fa:2` split into RD and WR islands. All existing data edges and
IMCL/control parameters must retain their bytes. No such graph has been
opened by this experiment.

## Endpoint bodies, events and metadata

The primary API contracts are the public
[RD endpoint](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/modules/rd_sh_mem_ep_api.h)
and [WR endpoint](https://github.com/Audioreach/audioreach-engine/blob/5773bf8e627c9857481dd338a9ec4aea4a49afd3/fwk/api/modules/wr_sh_mem_ep_api.h).

| Operation | Opcode / payload |
| --- | --- |
| Submit empty RD buffer V2 | `0x0400100b`, 32 bytes: data `{address low, high, map handle, capacity}`, then the same four metadata words |
| RD_DONE V2 | `0x05001005`, 56 bytes: data status, address low/high, handle, size, timestamp low/high, flags, frames; metadata status, address low/high, handle, size |
| Submit filled WR buffer V2 | `0x0400100a`, 44 bytes: data address low/high, handle, size; timestamp low/high; flags; metadata address low/high, handle, size |
| WR_DONE V2 | `0x05001004`, 32 bytes: data address low/high, handle, status; metadata address low/high, handle, status |
| RD configuration | `0x08001007`, `{num_frames_per_buffer=1, metadata_control_flags=1}` |

DONE parsing must follow GPR source/destination domain, exact endpoint/client
port, packet length, graph epoch, unique live token and slot-state checks.
The helper validates only the payload and submitted address/handle/capacity;
it cannot establish any outer identity or lifetime. RD needs one successful
nonempty frame and bounded metadata. Both WR statuses must succeed before
reusing its slot. Unchanged timestamps are signed 64-bit microseconds; adding
encoded TTP must not change them. WR EOF is bit `0x10`, timestamp validity bit
`0x80000000`.

The OLC frame-info writer is present in this firmware: vtable at `0xb07923e4`
reaches `0xb0262248`, which creates metadata `0x0a001052` with frame size,
timestamp low/high and validity flags before calling the OLC metadata writer.
That supports the static single-frame design, but actual returned RD metadata
has not been observed. The transform accepts only reviewed Bluetooth
`0x0a001037/38/39` and one full-buffer frame-info record. Tracking, token,
extension, EOS, DFG, flush and unknown records still reject the entire buffer.
Opaque byte preservation alone does not prove metadata lifetime safety.

OLC media-format propagation can use `0x03001009`; another RD path uses
`0x06001000`. Register and validate the actual direct event opcodes, including
operating-frame-size `0x03001004` and RD EOS/discontinuity `0x06001001`.
Do not assume every event is wrapped in `APM_EVENT_MODULE`. Public
`cu_raise_module_events_to_clients()` routes the registered client's domain,
port and token and sends the event ID as the packet opcode. A future owner
must process offload state/property events as well as media format. Raw
compressed format must be acknowledged at WR before any filled WR buffer.

The reviewed WR metadata loop advances by `24 + payload_size`, without
rounding. The bounded transform therefore requires each reviewed payload's
natural four-byte alignment. No generalized padding or unknown-record
forwarding is authorized by this finding.

## Reproducible public references

Public engine revision: `5773bf8e627c9857481dd338a9ec4aea4a49afd3`.
Public graphservices revision: `558ddfc2c87019496957855c209dd5ece4f6aceb`.
The actual firmware and library address observations above are tied to these
SHA-256 identities; they are not assumed portable across updates:

| Local artifact identity | SHA-256 |
| --- | --- |
| `libar-gpr.so` | `1339b2bcda1415535ec53666ceb4f873c3a7d33faf2a3cb55af3702a0a306d15` |
| `libar-gsl.so` | `4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc` |
| Analysis sections reconstructed from stock ADSP | `de7ec2ab21308fbce5f5c87c845fd2995e8367f3a39978c6bde63573969dd3b6` |

Run `python3 experiments/latency/python/endpoint_wire_protocol.py` and
`python3 experiments/latency/transport/test_endpoint_wire.py` for entirely
offline checks. They do not establish live callback ownership, an accepted
offload parent, cache coherence, receiver playout delay or increased quality.
