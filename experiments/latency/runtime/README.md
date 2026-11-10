# Embedded runtime evidence and ownership

This adapter runs inside the existing DZIF audio HAL's GSL process. It reuses
the owner's GPR registry, reader and descriptor. It does not initialize GPR,
open audio-pkt, initiate global GPR teardown, or modify the signed DSP encoder.
The optional builder/package does not request activation by itself.

Positive delay currently targets the reviewed R3,44.1 kHz,Adaptive HQ profile.
A successful graph OPEN is only one prerequisite. Applied status additionally
requires accepted encoder INIT, PREPARE/START, reviewed framing, a fresh root
lease, and completed WR buffers containing the matching positive TTP change.
Saving a setting, sending a buffer, or receiving an OPEN ACK is insufficient.

Original GSL source ports are identified by successful registration of the
exact stock callback and graph context. Their numeric range is not an ownership
proof. Deregistration revokes authority before calling the original function;
a failed deregistration or later re-registration cannot revive a frozen epoch.
The private APSS client port remains separate from the original source port.

Control responses are copied before the stock signal wakes its waiter and
checked against the full outgoing token and reversed source/destination.
Only a CLOSE covering the target SG can authorize retiring its mappings.
Unknown or incomplete close evidence leaves them quarantined until independently
confirmed DSP reset. STOP retains mappings for a later START on the same epoch.

## Diagnostic snapshot

The current payload has a512-byte diagnostic prefix in its private state.
Its symbol RVA must be read from the exact built ELF; it is not an ABI promise.
The current receipt-trace build places it at0x5c000. A read-only process-memory reader may
copy it only after pinning process start time, exact library identity, readable
VMA bounds and an even,unchanged sequence. No ptrace, process suspension,
ioctl, GPR client or memory write is needed.

| Offset | Bytes | Meaning |
| --- | --- | --- |
|0|8|SHZDBG1 magic |
|8|4|record bytes512 |
|12|4|snapshot sequence |
|16|4|diagnostic ABI1 |
|20|4|last bridge action |
|24|8|APSS monotonic time in ns |
|32|8|last relevant ACDB caller RVA |
|40|8|original graph identity |
|48|8|HAL context address |
|56|24|source,processor,scope flags,open/start/stop refs |
|80|16|bridge phase,transport-ready,lease,context-copy flags |
|96|64|copied HAL mode context, or latest-buffer SHZLST1 when flags at92 equal2 |
|160|8|SG/key counts |
|168|32|bounded SG-ID prefix |
|200|128|bounded GKV-pair prefix |
|328|64|last16 ACDB caller RVAs |
|392|64|BST1 startup snapshot; transient INIT diagnostic before the next worker snapshot |
|456|56|last significant APSS GPR header and up to32 payload bytes |

The receipt trace is bounded, nonblocking and copied before the original packet
is freed. It is diagnostic evidence only; it cannot authorize a transform,
mapping retirement, applied status or a retry. These records may contain private
process addresses and are not included in ordinary user status exports.

The BST1 snapshot contains startup flags, RD operating-frame bytes, initial peer
ACK mask, last accepted RD state/count, pending control, accepted format bytes,
transport error/phase, unresolved buffers/maps, context sequence and a monotonic
timestamp. It records which prerequisite is missing without changing it.

After an actual CPU-owned buffer reaches validation, offsets168..327 may
instead contain a SHZBUF1 record: exact epoch, data/metadata lengths, original
parser status, profile/delta, first8 wrapper-header bytes and up to112 metadata
bytes. SG/key counts become zero in that view. The first timing packet may be
replaced by the first timing-free supplemental packet, then by the first
parser failure in that epoch. This retains the supplemental packet's control
metadata without retaining the encoded audio body.
The encoded audio body
and live packet pointers are not retained. Every span is bounded by the owned
slot's actual capacities. This record cannot authorize unknown metadata or a
retry; the original complete validation and ownership rules still apply.

After any actual owned buffer is validated, offsets96..159 hold the latest
SHZLST1 record and the context-copy flags at92 become2. Its fields are epoch
at104, data/metadata lengths at112/116, parser result at120, outgoing WR flags
at124, requested delta at128, first8 original wrapper bytes at136, outgoing
timestamp at144 and wrapper/changed-wrapper counts at152/156. The earlier
SHZBUF1 record remains the first observation or first later failure; SHZLST1
permits comparing a subsequent supplemental packet without keeping audio
content or live pointers. This overlay is diagnostic only and never replaces
the separately frozen, validated mode context used for authority.

An independent512-byte SHZDTR1 sidecar follows the diagnostic prefix, so a
1024-byte read can include both. Its sequence at sidecar offset16 must be
checked separately before and after copying. The sidecar retains the first
eight RD_DONE, WR_DONE or negative BASIC receipts for one increasing graph
epoch: ports/token/opcode, statuses, lengths, frame/timestamp flags, receipt
time and bounded malformed-shape flags. It retains no audio, addresses, map
handles or live pointers. An empty sidecar means no selected receipt was
captured. These untrusted observations never authorize a completion or retry;
the transport still independently validates the complete ownership tuple.

## Verified INIT mode packing

The exact reviewed binaries are:

- `btaudio_offload_if.so`: SHA-256 `1cb1e93a0278b6d1cd1fd84f1c97ccfe28a7b19e10837d55cc92bae5fea213da`.
- `lib_bt_aptx.so`: SHA-256 `a736bb925aecb0ee1664d6273a3b38808dbbc9ad5d3790694aa2a688f99037f4`.
- Signed `aptx_adaptive_enc_module.so.1`: SHA-256 `05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3`.

Samsung accepts the initial mode at `0x141dc..0x141fc`. With
`aptxadaptiver2_1_support=true`, `0x1434c..0x143b4` augments that word with
`sinkCapability[0] << 8 | sinkCapability[1]`; `0x14588..0x145a0` logs the result.
The aptX plugin copies it into INIT offset36 at `0x6b70..0x6bb4`.
Thus the observed, successfully acknowledged INIT mode `0x1197` is
`HQ0x1000 | (0x01 << 8) | 0x97`. Comparing the complete word to4096 rejects
this legitimate packed configuration.

The signed DSP wrapper checks capability presence at `0x13d30`, selects R3
when `(INIT[49] & 0x82) == 0x82` at `0x14048..0x1405c`, and additionally requires
sample-rate enum2 at `0x14160..0x1416c` before R3 initialization at `0x141cc`.
`0x142a8..0x142c8` dispatches to the R3 vtable for the mapped44100Hz rate.
Legacy HQ selection also masks profile with `0x3000` at `0x7c38..0x7c50`.

The host gate requires exactly59B, rate enum2, capability0 exactly1,
capability1 containing0x82, and raw mode exactly
`0x1000 | (capability0 << 8) | capability1`, together with the independently
attested non-Lossless HQ44100 context. Pure0x1000 remains unsupported by this
gate. A matched successful INIT ACK establishes configuration acceptance;
the first actual buffer must still pass complete R3 framing validation before
any positive TTP rewrite or WR submission. INIT alone is not applied-delay proof.
