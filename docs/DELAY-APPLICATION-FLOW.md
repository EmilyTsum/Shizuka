# Native delay application transaction

**Portable policy, an isolated launcher and Android service adapter source are
implemented; production qualification remains false.** The encoded transport
is still undergoing device validation. See the
[source and test results](../experiments/latency/application/README.md) and
[remaining root-owner hookup contract](../experiments/latency/application/INTEGRATION.md).
The compiled default adapter and fixed executor cannot mutate audio. The native
Apply action and literal control facade are integrated separately and report
unavailable while these gates remain false. Save continues to mean pending.

## Problem and existing boundaries

`MainActivity.editDelay()` saves a typed request through `config.sh tuning set`.
The runtime freezes its accepted delta for a graph epoch, so later publisher
revisions correctly remain pending. `apply.sh` and `boot-activate.sh` can keep a
verified active session; running them is not proof of a new audio epoch.

The authenticated native bridge already permits literal `control` arguments.
No shell command construction, additional root grant, TCP endpoint, public
configuration file or WebUI setting is needed. The native app must continue to
distinguish a saved request, an accepted lifecycle operation, and observed TTP
application.

`settings-bridge.sh start` stops the previous bridge, and
`SettingsBridge.close()` terminates its direct command children. A long
synchronous reconfiguration command could therefore be killed when the user
reopens KernelSU Action. The operation must be an independently supervised,
bounded module worker, with a short launch request and automatic status reads.

## Proposed public operation

One fixed root entry point, using the existing bridge:

```text
control.sh latency-apply OPERATION_UUID EXPECTED_HQ_MS EXPECTED_LL_MS
```

The UUID is an idempotency identifier, not an authorization credential or the
packaged runtime cookie. HQ and LL arguments must exactly match the current
validated saved values. The proposed app call is a short request, for example:

```java
api.json("control", 20, "latency-apply", operationId,
    Integer.toString(savedHqMs), Integer.toString(savedLlMs));
```

The native app uses this explicit short action and automatic status polling.
Once qualified, the control dispatch validates authority and the request,
records a private operation, launches its bounded worker, then returns a final
JSON line promptly:

```json
{"schema":1,"operation_id":"UUID","accepted":true,"phase":"queued","applied":false}
```

A valid but unsupported request returns structured `accepted:false`,
`applied:false` and a concrete reason. Invalid syntax or authority remains a
command error. Repeating the same UUID and values returns the existing
operation. Reusing an identifier with different values is rejected. A second
operation cannot run concurrently.

Poll `control.sh latency-apply-status [OPERATION_UUID]`; omitted UUID means the
current/latest operation. Implemented phases are `queued`, `preflight`,
`reconfiguring`, `restoring`, `waiting_for_playback`, `verifying`, `applied`,
`disabled`, `superseded`, `rejected`, `recovered`, `unconfirmed` and `failed`.
No journal returns `saved_pending`. Launch acceptance never sets `applied:true`.

## Root transaction

1. Acquire the shared lifecycle lock and verify canonical module/state paths,
   root authority, boot, removal/disable markers, packaged runtime identity,
   native/HAL/GSL hashes and reviewed dependency hashes. Reject unresolved
   ownership/recovery state. The worker rechecks these after acquiring its own
   lock; the launcher must not accidentally pass the lock FD to detached work.
2. Capture the exact current context: unique connected active bonded A2DP peer,
   actual codec configuration, media route/audio mode, known playing or idle
   state, and an unambiguous media-session owner if playback restoration would
   require controlling it. Reject call mode, ambiguous peers, missing state or
   unvalidated transport contexts before mutation. Preserve the current mode;
   do not silently choose HQ44.1 to manufacture a supported context.
3. Record an owner-only atomic operation journal: UUID, boot, packaged cookie,
   config identity, captured process identities, prior epoch tuple, private peer
   identity, codec/playback snapshot, worker PID/start ticks and deadline. Keep
   addresses and media-session tokens out of ordinary UI snapshots.
4. Arm operation-owned recovery while retaining the old health watcher.
   Coordinate provider stop, old owned graph/buffer teardown, and health-token
   replacement after new readiness through the existing verified paths. A delayed
   old watcher must not roll back the new generation. Unknown ACK/ownership
   outcomes remain failures requiring recovery, rather than permission to reuse
   buffers or discard ownership records.
5. Use the least disruptive **device-validated** recreation method. Ordinary
   pause/resume is insufficient because it retains the graph epoch. A HAL-only
   recreation may preserve the Bluetooth process and codec, but its fresh-graph
   behavior has not been established here. A2DP route deactivate/reactivate is
   another candidate, not proof of graph destruction. Never route audible
   playback to the speaker while cycling the Bluetooth route.
6. If the approved policy requires a full Bluetooth reconnect, preserve the
   existing payload/namespace semantics, prepare the current-boot request from
   the verified packaged cookie/hash, and recreate using the lifecycle owner's
   controlled path. Reconnect only the captured still-bonded peer. Never choose
   the first bonded device, start pairing, enable an initially disabled radio,
   or change to another active peer.
7. Restore the captured codec only after connection/activation readback and
   advertised-capability validation. Start the new provider with the new actual
   HAL PID/start identity and establish the new health generation. Preserve
   idle playback: no test tone, global media-play event or automatic Play for
   an initially idle session.
8. For an initially playing session, restore only its same identifiable local
   media owner when the snapshot is still current and the operation caused its
   pause. Abort restoration on a changed peer, codec preference, media owner,
   track/session or conflicting playback transition. Unknown intent must not
   cause old settings to overwrite a later user change. Do not use a global
   media key that could control a different session. If the required media
   authority/identity is unavailable, reject before interruption or preserve
   pause and report that playback restoration could not be confirmed.
9. Observe the new epoch and typed delta. Finish as applied only on the evidence
   below. Idle completion is `waiting_for_playback`, with `applied:false`; the
   user can resume normally and the automatic status view will confirm it later.
   A failed attempt restores the verified prior route where ownership is known,
   retains the user's latest saved configuration, and leaves a bounded recovery
   path armed. This action never commits an unheard experimental payload.

The worker must have its own deadline and module/root/boot checks. Reopening the
UI can reconnect to its journal without starting a second operation. Module
disable/removal or authority loss stops mutation and enters the established
recovery path. The worker cannot depend on a live activity or bridge socket.
`setsid` alone is not evidence that it survives Android process-group/cgroup
cleanup. The observed diagnostic stale-file case makes whole-tree termination
an explicit recovery test; the lifecycle owner must establish an independent
supervision context rather than assume detachment is sufficient.

## Codec and user-change preservation

`BtPlayback.select` is an explicit preference builder: it chooses advertised
flags, highest priority and stereo, and writes codec-specific word 1. It does
not clone words 2–4. It must not be reused as an exact restoration primitive.
The new session helper must preserve the actual extended codec type, priority,
rate, bit depth, channel mode and all four opaque codec-specific words, then
read them back. Never turn the current HQ44.1/24 into HQ44.1/16, or replace the
dedicated Lossless selection with a similarly named Adaptive type.

Bind the operation to the saved request and recheck it before mutation and
before declaring success. A changed configuration supersedes that operation;
do not rewrite the old file to force a match. A changed user selection likewise
ends restoration authority. Lifecycle-generated disconnect/default-codec states
must be distinguished from a later deliberate choice; inability to distinguish
them is an unconfirmed restoration, not permission to overwrite either.

Platform APIs cannot reveal every redundant user command: for example, another
Pause while already paused may produce no distinguishable state transition.
The implementation must document its observable change fence and use a
conservative resume policy when ownership or intent is uncertain.

## Applied evidence and automatic UI

The packaged cookie can remain identical across sessions. A HAL restart can
also restart the numeric graph counter. A new epoch therefore means a new
bound tuple of boot, packaged cookie, HAL PID/start and graph epoch; a nonzero
or different numeric epoch alone is insufficient. Keep operation identity and
provider ownership/revision binding alongside this tuple.

Use the existing strict provider/status proof: fresh valid lease and process
binding, ACTIVE phase, accepted INIT/OPEN/START, reviewed actual profile/rate,
healthy worker/registration, no error, completed RD/WR and, for positive deltas,
WR_DONE-confirmed rewritten wrappers. The latest TTP difference must equal the
requested frozen delta. The graph must belong to this operation's new epoch,
and the saved request must still match. A healthy worker, a config save, an
encoder bitrate estimate or a successful restart is not that proof.

Zero bypasses the production transform. Its application uses owned removal and
a separate fresh normal-owner receipt, reporting `disabled`, never `applied`.
The old relay must be removed, stock GSL verified, the request current and a
fresh normal session bound to this operation. It can preserve a normal 96 kHz
selection without claiming positive-delay support for it, and requires no WR
rewrite or provider lease. Persisted remote uncertainty must be resolved by the
verified lifecycle owner; a changed HAL PID, expired lease or missing status
cannot clear it. LL intent and other unvalidated positive contexts remain
pending.

The app retains **Save** with “Saved · Pending application.” An explicit
**Apply now** action appears/enables only when a separate backend application
capability and current-context preflight authorize it. Current
`runtime_apply_supported` is derived from an already-applied observation and
must not be treated as this new lifecycle capability. The action's description
states the approved interruption/reconnect behavior before the user taps it.
No additional root permission prompt or manual status-observation button is
required.

Status remains automatic. Show requested, currently observed, and operation
progress separately. After an idle recreation show “Ready for next playback.”
After an unknown IPC outcome recover the same UUID's status; do not launch a
new mutation automatically. UI callbacks must check root-session generation
and operation identity so an old response cannot replace a later request.

## Acceptance checks before enabling the action

| Scenario | Required result |
| --- | --- |
| Save during a running old epoch | New request is pending; old observed delta remains explicitly old. |
| Repeated launch UUID / competing UUID | Same operation reused / second operation rejected; no duplicate restart. |
| Unsupported rate, LL or unvalidated runtime | No lifecycle mutation and no codec switch; saved request retained. |
| Already-applied old graph or reused numeric epoch | Cannot confirm the new operation. |
| Initially idle | No Play or tone; waiting-for-playback status. |
| Initially playing, same identifiable owner | Bounded interruption and verified restoration to that owner only. |
| User changes codec, track, peer, config or play-state | No stale restoration/write; report superseded or unconfirmed. |
| Peer absent/ambiguous or capabilities changed | No pairing or alternative-device selection; recover safely. |
| IPC loss or reopening Action | Same supervised operation remains queryable; no duplicate mutation. |
| Worker death, expired lease, module/root/boot loss | Bounded owned recovery; no applied claim. |
| Whole Android process-group termination | Independently supervised recovery is proven, or that launch arrangement is not enabled. |
| Missing WR_DONE, unknown ACK/metadata or stale status | Remain unconfirmed and preserve ownership rules. |
| New active epoch + matching delta + completed rewrite proof | Applied for this request, with explicit observation scope. |

## Platform references

[AOSP's pinned A2DP API contract](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/57b823f4f7/framework/java/android/bluetooth/BluetoothA2dp.java)
describes active-device changes and stopping Bluetooth streaming; it does not
promise destruction of this Samsung DSP graph. The proposed epoch method still
needs actual-device validation.

[MediaSessionManager](https://developer.android.com/reference/android/media/session/MediaSessionManager#getActiveSessions(android.content.ComponentName))
provides controllers for active sessions and requires media-control authority
or an enabled notification listener. This proposal does not add a notification
listener or claim that the current root bootstrap has already passed that API's
permission check.
