# Live transport and temporary-root session contract

This specifies requirements for a future owner. The file transform implements
no live GPR transport, graph creation or teardown.

## One HAL transport owner

The reviewed device `audio_pkt_dlkm.ko` SHA-256 is
`30e3c82b05d74239ac0f3261a30866b6e330368ba30236dd5be4c6188a168ab6`.
Its queue is shared across file descriptors, and release unconditionally calls
`spf_core_apm_close_all` and `msm_audio_ion_crash_handler`. Thus a nominally
read-only standalone GPR helper can stop graphs when it exits. A second
audio-packet descriptor, independent reader or helper-owned GPR initialization
is forbidden. The relay must register through the running HAL's existing GPR
registry and use its existing response dispatcher. It must never close the
HAL's transport or consume another client's packets.

The private probe's MAP was accepted, but the existing HAL consumed the UNMAP
basic acknowledgement for port `0x535a6201` and logged an unknown port. Its
status and remote cleanup remain unconfirmed. Neither process exit nor a
timeout proves unmapping. Do not reuse that handle or repeat a command whose
completion is unknown.

## Session stages

1. **Attach.** Bind one owner to the boot ID, HAL PID/start identity, module
   generation, active root-session authority and existing GPR dispatcher.
   Register only provably owned client ports. Missing authority, an unknown
   port namespace or an unreviewed firmware identity prevents activation.
2. **Prepare a normal fresh graph.** Bind the exact provider revision, graph
   keys, SG/container/module identities, actual INIT context and graph epoch.
   The reviewed Adaptive target is SG `0xb00000ab`, container `0xe00000e6`,
   encoder `0x43fb` → logger `0x43fa` → connection proxy `0x4098`. Preserve its
   IMCL links. RD/WR require OLC/offload-info setup. Static firmware analysis
   identifies parent-container property `0x080010cb` as its creation path;
   APSS-as-parent and same-container disjoint-path pacing remain unvalidated.
   A size query and its blob fill must describe the same revision. Failure
   after advertising extra space aborts this open.
3. **Own mappings.** Allocate bounded private slots, record their actual OSAL
   mode/FD/capacity and obtain acknowledged APM map handles through the same
   dispatcher. A handle belongs to this generation only. Offset zero is valid
   in the reviewed offset mode. No endpoint data may be sent before graph,
   format, mapping and cache ownership are established.
4. **Transfer complete buffers.** Follow the transitions below. Each request
   has a unique live token and expected endpoint, capacity and graph epoch.
   The dispatcher validates domains, ports, opcode, size, status and ownership
   before delivering a completion. Unknown, duplicate or stale responses do
   not advance a slot.
5. **Drain and close.** Stop submissions, resolve owned in-flight buffers,
   close only this route through the HAL's ordinary graph lifecycle, confirm
   owned unmaps, then free memory and unregister the owned ports. No global
   APM close-all, transport release or blind retry is a cleanup mechanism.

## Buffer and cache transitions

Use separate dma-bufs for independently owned slots: DMA cache synchronization
covers the allocation, so two owners cannot safely divide one allocation.
Data and metadata spans within a slot are bounded by its recorded capacity.

| State | Required transition |
| --- | --- |
| CPU owned | The OSAL allocation has CPU START ownership. Complete host preparation, then issue CPU END before RD submission. |
| RD in flight | DSP owns the slot. Host must not read, modify, free or submit it again. |
| RD complete | Accept the matching successful RD_DONE, then CPU START. Validate complete data and metadata. Read one immutable configuration snapshot and positively gate the actual profile. |
| CPU transform | Validate every wrapper before publishing output. Use that one delta for all wrappers, preserve secondary zero TTP and reject overflow/unknown metadata atomically. No config reread between split-channel wrappers. |
| WR in flight | CPU END precedes WR submission. Keep data, metadata and the snapshot alive until matching successful WR_DONE acknowledges both. |
| Reusable | Only confirmed WR completion releases the slot. Establish CPU START before any further host access or vendor free; a later DSP submission requires CPU END again. |

A malformed buffer or bad configuration aborts this relay epoch without
partially sending it. Do not silently retain an older positive delta. A profile
or graph change invalidates the epoch; HQ attestation does not authorize LL or
Lossless. Display the requested setting separately from the transformed packet
TTP and measured receiver behavior.

Atomic buffer snapshots alone do not establish that an arbitrary delay change
is safe during playback. The initial live provider must freeze the accepted
delta for a graph epoch; a new saved value remains pending until a fresh owned
session. Each buffer uses that accepted immutable value. Midstream updates need
separate receiver/timeline validation, especially when reducing the delta.

The file reader is a root-side component: `/data/adb/shizuka/config.conf` is
root-owned mode 0600 inside a mode-0700 directory. The actual HAL runs as
`audioserver`, so an in-HAL owner cannot read that file directly. A future
authenticated root/HAL provider must deliver an immutable typed snapshot bound
to the current session and revision. The buffer worker retains that snapshot
through WR completion; missing or expired authority rejects further work.
This provider is not implemented. Do not weaken file permissions, grant the
HAL root or treat successful root-side file tests as proof of this boundary.

## Removal, expiry and uncertain cleanup

Module disable/removal, root-session loss, authority expiry, HAL replacement,
graph replacement or an endpoint error moves the owner to draining. Opening
KernelSU Action only opens the module-owned UI; it does not open a relay graph
or grant the app independent root. A reboot invalidates every previous owner,
token and mapping record. Late-load after temporary root requires fresh checks.

Drain has bounded waits. A timed-out RD/WR/UNMAP remains **cleanup unknown**;
it does not permit buffer reuse, free, re-registration of the same generation
or a fresh experimental activation. Keep the unresolved ownership evidence,
reject further relay work and use the existing HAL/module recovery lifecycle.
Never infer success from a disconnected UI, closed FD or vanished process.
Only an acknowledged cleanup or independently established DSP reset can
discharge the unresolved remote lifetime.
