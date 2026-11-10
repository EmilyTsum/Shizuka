# Fixed owner integration contract

## Current implementation boundary

The portable command/engine/store/status, root guardian channel, real Android
service adapter and fixed mutation ABI are source and host-testable. The
compiled default entry still selects `UnqualifiedApplyAdapter`, and
`module/latency-apply-executor.sh` exits 10 behind its literal false source gate.
The concrete factory, read-only adapter and resident event loop are implemented.
The product facade and native
UI hooks are integrated separately; they do not enable this factory. No
application executor source here was run on a phone. Qualification remains an
explicit coordinated source change after actual continuous transport and
supervision verification.

The concrete device-specific read-only implementation is `ReadOnlyRootFacts`
with `InstalledRootSource` and the pure `RootFacts` evidence model. It uses the
fixed installed helpers, private configuration and raw lease/status records;
it does not use optimistic defaults, an INIT ACK alone or a file-presence
enabling flag. Factory, Facts capability, receipt-writer and fixed-executor
source gates remain false. Completed RD/WR, recovery and real supervision must
be qualified before enabling them together.

## Public hook, once qualified

Add only literal cases to the existing `control.sh` dispatch, outside its normal
lock-acquisition set. The new launcher acquires that same lock itself:

```sh
latency-apply)
 shift
 exec "$BB" sh "$MODDIR/latency-apply.sh" begin "$@"
 ;;
latency-apply-status)
 shift
 exec "$BB" sh "$MODDIR/latency-apply.sh" status "$@"
 ;;
```

Public arguments are `latency-apply UUID EXPECTED_HQ_MS EXPECTED_LL_MS` and
`latency-apply-status [UUID]`. The authenticated bridge already passes literal
`control` arguments. Do not add a shell-evaluation entry or another root grant.
The short begin response is final JSON, and repeated UUID/pair is idempotent.
The same UUID with different values is invalid. An unresolved owner blocks a
different operation. No cancel API exists; interruption/loss is recovered by
the bounded owner path rather than an automatic second mutation.

Ship the separate source-bound `latency-application.dex` + 65-byte newline SHA
sidecar and the three new launcher/guardian/executor shell sources only after
qualification. The normal policy/status DEX and APK build remain separate.
The app uses its root-session generation and a UUID saved before submitting;
after an uncertain response it polls that UUID. It never retries a mutation
with a fresh UUID automatically.

## Guardian ABI and inherited lock

The launcher briefly acquires `state/lifecycle.lock` FD9. A queued worker starts
without that FD or a bridge pipe, then its guardian takes its own FD9 and keeps
it while Java receives FD9 closed. The worker has a 180-second transaction,
bounded recovery, and an outer 215-second timeout with 5-second kill allowance.
Android process-group/cgroup survival and orphan cleanup remain qualification
requirements; setsid and host process tests do not establish them.

Java writes one atomic 0600 request into the fixed 0700 directory
`state/latency-application/channel.UUID/`. The canonical ASCII line is:

```text
SHZAPPLY1 SEQUENCE ACTION UUID HQ_MS LL_MS DEADLINE_MONOTONIC_MS SNAPSHOT_TOKEN CONFIG_SHA256 FLAG
```

The snapshot token and config hash are 64 lowercase hex characters. Sequence is
1–10000, monotonically increasing. HQ is 0–150, LL 0–100, flag 0/1. There is no
path, command, codec-selection argument, peer address or arbitrary text. The
guardian validates canonical spelling, field count, UUID, next sequence,
regular-file owner/mode/link count and size before dispatch. Malformed requests
remain unresolved and cannot reach the fixed executor.

| Action | Meaning | Flag |
| --- | --- | --- |
| 1 | ARM operation-owned recovery and immutable capture binding | 0 |
| 2 | RECREATE owned route with current request | 0 |
| 3 | REMOVE owned relay and restore normal route for HQ0 | 0 |
| 4 | FINISH, only after owner verification | 1 if initially idle/waiting |
| 5 | RECOVER root-owned audio resources | 0; never a global/user Play request |

The guardian invokes one fixed file with eleven literal arguments:

```text
latency-apply-executor.sh ACTION UUID HQ LL DEADLINE TOKEN CONFIG FLAG GUARDIAN_PID GUARDIAN_START_TICKS SEQUENCE
```

It retains/inherits FD9. Exit codes are exactly 0 success, 10 unavailable,
20 failure and 30 persisted owned recovery required. Unknown exit codes become
20. A canonical response contains magic, sequence, action, UUID and status. The
old request is removed before publishing the response so the next request
cannot be deleted by the previous completion. The Java client rejects wrong,
future or malformed responses and leaves timeout ownership unresolved.

The fixed executor remains disabled. Its implemented private destination is:

```text
control.sh latency-owned ACTION UUID HQ LL DEADLINE TOKEN CONFIG FLAG GUARDIAN_PID GUARDIAN_START_TICKS
```

That handler must reuse inherited ownership. **Calling ordinary activate or
rollback as a child while FD9 is held deadlocks.** Verify root, canonical module
and boot, packaged complete source/runtime identity, current journal UUID and
pair, guardian PID/start/ancestry, its bound private owner record, and the
inherited FD9 inode against the same lifecycle-lock object. Do not accept a
bare environment flag or the mere presence of FD9 as proof. Do not reacquire a
new FD and infer that success proves prior ownership. The handler must enforce
the monotonic deadline and permit cleanup action 5 during module disable/remove
without permitting a new arm/recreate.

The guardian retains its local sequence for request/reply matching; it is not
forwarded as an argument to `latency-owned`.

The executor's dormant forwarder takes the eleven local guardian fields and
passes exactly the first ten as literal arguments to that fixed control entry.
Its local sequence is bounded/canonical and never forwarded. `QUALIFIED=0`
returns exit10 before any command or filesystem access; environment variables
cannot enable it. Host forwarding tests only arm a disposable source copy with
a synthetic control spy and retain FD9. They do not qualify the production gate.

`control.sh latency-owned` forwards to `latency-apply-owned.sh execute` with
those ten arguments. `latency-owned-health` calls `validate-health` with the
same values before rotating the existing health owner directly. The minimal
`latency-runtime.sh request-owned` path reuses the normal request validation
body after `latency-apply-owned.sh validate-request`; it does not reacquire FD9.
All three handler modes remain behind the literal `QUALIFIED=0` source gate.
No environment variable can enable them. The handler's positive recreation is
HAL-only: retain Bluetooth PID/start, revoke any old runtime lease, recover
remote ownership, unmount only the exact tracked GSL overlay, prepare the
reviewed request, mount and start a fresh fully verified HAL, then rotate health.
The existing health watcher remains alive while FD9 is held. No ordinary
control activate/rollback, Bluetooth restart, global media key or second GPR FD
is called in this path.

## Read-only facts and capture binding

The default entry checks `ApplicationQualification.residentFactory()` before
loading the one fixed `AndroidApplicationFactory` class. Both source gates are
false. The class name and DEX path are fixed; no caller-supplied class/executor
path or alternate classpath exists. `ApplyInvocation` validates every command,
UUID, delay, guardian PID and start-tick field before any Android service path.
Prepare/status use `ReadOnlyApplyAdapter` and never construct a guardian channel
or retain a MediaSession.Token. The current-target check remains an eligibility
fact, not profile evidence.

For the resident worker, `ApplyEntry` authenticates the actual root generation,
guardian tuple, current UUID and still-unbound QUEUED journal within its
20-second launch window before service startup. The fixed factory additionally
checks the source-bound module and actual guardian ancestry/cmdline plus its
FD9 inode. Java must not hold that lifecycle lock itself; ART may reuse numeric
descriptor 9 for an unrelated file. The worker rechecks this authority/queue
after service startup before the engine binds and can arm/pause anything.

`AndroidApplyBootstrap` checks the operation's corresponding positive/removal
qualification before connecting services. Startup is bounded to three seconds.
It accepts one A2DP service identity; disconnection or replacement cannot revive
a captured transaction, and late/duplicate callbacks cannot complete twice or
leak its accepted profile. The transaction runs off the main loop. Local session
and profile resources close before final completion returns on the main loop.
`LatencyApplyMain` writes one final result and exits `app_process`. Android's
main Looper does not allow quit/quitSafely, so those methods are never used.
There is no resident token in a later status process or orphan finalizer.

`ReadOnlyRootFacts` implements these typed methods:

* `qualified` / `disableQualified`: complete fixed-executor qualification,
  separate positive-route and owned-removal capabilities.
* `inspect`: current root/boot/module/guardian authority, packaged cookie,
  config identity, actual HAL PID/start/epoch, actual reviewed HQ/rate/profile,
  normal-removal capability and `ownedRecoveryRequired`.
* `requestCurrent`: full canonical private configuration validation and exact
  HQ/LL pair/hash comparison. Reuse the existing strict typed reader/policy;
  do not parse only two lines and ignore invalid surrounding fields.
* `snapshotCurrent`: later read-only status processes verify the root owner's
  private captured peer/codec/epoch binding. A later process has no resident
  MediaSession.Token and must not try to restore playback from a journal.
* `observe` / `disabled`: return authenticated operation/snapshot/config-bound
  current evidence, using the existing status reader and ownership journals.
* `resetPermit`: an exact operation-bound, new-process-owned reset codec parcel
  fingerprint plus the allowed Android codec-event sequence. A mismatching
  current codec is never overwritten merely because a reconnect occurred.
  This returns null until a real reset-origin API is available.
* `resolved`: independently verify lifecycle completion or recovery. A zero
  exit code from the executor is insufficient on its own.

Before ARM, Java atomically stores a bounded private `capture.bin`: version/CRC,
UUID/pair/token/config, old boot/cookie/HAL/epoch, captured bonded address,
verified framework codec parcel and original play/media identity. The root
handler must validate all bindings, ownership, size and corruption checks, pin
its digest/identity in the owner journal and reject a later replacement. Never
deserialize an external arbitrary class. The live MediaSession.Token remains
in Java memory; it is not written to this file or UI JSON. An orphan recovery
can restore owned audio resources but cannot use an old token or global media
key to resume an unknown session.

The unreleased capture ABI is version 2: the complete typed original snapshot
is included before the peer/parcel/media data. `ApplicationCapture` validates
its CRC/size/fields without deserializing an arbitrary class. This permits the
root writer to validate ARM while the application journal is still PREFLIGHT.

## Owner receipt and fixed helper commands

`state/latency-application/owned.UUID` is a root0600 single-line canonical record:

```text
1 UUID BOOT COOKIE SNAPSHOT_TOKEN CONFIG_SHA HQ LL GUARDIAN_PID GUARDIAN_START OLD_HAL OLD_START NEW_HAL NEW_START GRAPH PHASE CAPTURE_SHA PACKAGE_SHA ACTIVATION_SHA
```

There are exactly 19 fields, lowercase hashes/UUIDs, canonical unsigned decimal
numbers and one newline. ARM requires new HAL/start/graph to be zero. READY
binds the actual new HAL and current activation, initially with graph zero.
FINISH freezes an actual proven positive graph. Phases are `armed`, `ready`,
`waiting`, `applied`, `disabled`, `recovered` or `unconfirmed`. APPLIED requires
positive HQ and nonzero graph; DISABLED requires HQ0 and graph0.

The package hash binds the canonical identity and complete fixed helper/core
artifacts, stock input manifest, application reader/normal/fence/owned helpers,
settings bootstrap/bridge/UI lifecycle, APK and digest. The activation hash
binds the current private health generation,
active marker identity and BT/HAL PID-start records. Legitimate payload chmod/
chcon updates rehash and verify the same immutable bytes; different code still
invalidates the package. A READY record with graph0 is not applied evidence.
Subsequent proof requires its exact new HAL/request/lease and actual completed
WR_DONE timing. A missing or malformed record cannot borrow another operation's
fresh epoch.

The fixed read-only CLI in the separate DEX is:

```text
LatencyRootFactsMain --module-dir INSTALLED_MODULE inspect
LatencyRootFactsMain --module-dir INSTALLED_MODULE observe UUID
LatencyRootFactsMain --module-dir INSTALLED_MODULE disabled UUID
```

Its scope is `application_root_facts`, and `qualified_executor`/`available`
remain false. An observed proof is a separate typed field, not an enabling
capability. `AndroidSelectionFacts` is a no-argument read-only child reader for
the full actual framework codec parcel and unique bonded peer fingerprints.
It performs no connection, preference or media writes. The fixed
`latency-apply-read.sh` recomputes existing reviewed preflight/normal readiness
hooks and live BT/HAL/APK/GSL identity without starting/stopping services.

The typed resident owner writer/finalizer takes the exact private handler data:

```text
LatencyOwnerReceiptMain --module-dir INSTALLED_MODULE ACTION UUID HQ LL DEADLINE TOKEN CONFIG FLAG GUARDIAN_PID GUARDIAN_START
```

Its compiled `QUALIFIED=false` returns exit10 and never writes. The transition
logic and guarded atomic publication exist behind that source gate. Before
publication it checks the inherited lock inode, guardian ancestry/cmdline/start,
private application journal and capture bindings, current config/boot/package,
allowed phase, actual new root session and evidence. A root owner success exit
alone cannot publish APPLIED. Normal recovery can confirm owned resources while
preserving a newer user config/codec; it never restores old intent to force a
match. Orphan/disable/remove reconciliation remains an owner qualification test,
not a public journal-reset command.

`target_eligible` is independent of `profile_observed`. The former requires
the fixed reviewed device/module/native pair, unique actual Adaptive HQ44.1/24
selection and exact matching advertised capability. It permits the first idle
attempt before an encoder exists. `current_context_supported` describes that
eligible attempt, not proof of an R3 profile. Actual profile evidence remains
false until accepted INIT, strict actual framing and matching completed WR_DONE
proof. No Android codec label supplies that evidence. The reset-origin permit
is absent; a different codec after HAL recreation cannot be overwritten.

The Android adapter's authority callback is checked in its bounded waits and
before writes. For a resident transaction it must include guardian PID/start
liveness as well as root/boot/module/current-operation identity. Missing media
permission, identity or unambiguous local player fails before interruption.
Restore preserves all codec fields and only the still-bonded captured peer.
Observable track/queue, media token/state-update marker, different peer or
codec-event changes abort stale restoration. Framework APIs cannot identify
every redundant manual Pause while already paused; do not claim otherwise.

## Recovery fencing and temporary root

Normal activation/commit/keep use `latency-apply-fence.sh check` under the same
lifecycle lock. Health uses `health`, which also verifies the resident guardian
and its bound timeout child with a bounded lifetime. Cleanup is not blocked.
An unresolved FAILED/UNCONFIRMED journal is not pruned to enable retries.

The separate normal-owner metadata finalizer is:

```sh
"$BB" sh "$MODDIR/latency-apply-fence.sh" reconcile
```

It requires inherited FD9 from canonical `control.sh rollback`,
`rollback-trial` or `rollback-health`, with exact lock inode and root/controller
ancestry. It never borrows a dead guardian's writer authority. Exit 0 means all
application metadata is resolved; exit 3 retains the fence. When records need
resolution its JSON scope is `latency_application_reconcile`, with `resolved`,
`unresolved`, `current_boot_id`, `reconciled_count` and a constant reason.
No-journal/empty-directory fast paths can return the ordinary clear fence JSON.

The fixed `latency-apply-normal.sh` independently checks stock DSP/GSL/loader,
all five stock input hashes in the relevant namespaces, and the full same-PID
HAL core/effect/Samsung binder set. Missing runtime/remote uncertainty markers
alone are insufficient. Same-boot reconciliation requires the old guardian and
captured HAL identities to be gone; a previous boot skips every old PID probe.
Only protected canonical journal/capture/owner/package bindings are accepted.
ARM stores its original reviewed runtime identity before the owner receipt;
a leftover request can be archived as private `channel.UUID/request.cancelled`
only when its full boot/cookie/GSL/mode line matches that captured identity.
Foreign or malformed requests remain untouched and unresolved.

The root-private `reconciled.UUID` JSON receipt is atomically fsynced before the
journal becomes RECOVERED. It binds the old record sequence/request/boot and
capture/package lineage, the current package/boot and independently verified
normal HAL. Interrupted finalization can resume safely. Historical receipts
are not current Applied or Disabled proof. This command does not signal PIDs,
restore a codec, change config, start services or press Play. Normal rollback
tries it after the generation guard and before physical cleanup. An app-only
unknown/orphan with no physical recovery markers may only retry this read-only
verification; failure cannot replay an old radio-on record, restore properties,
stop policy or restart audio. A missing DEX with any journal metadata stays
fenced.

Physical recovery and application metadata completion have separate markers.
After actual cleanup, the normal controller independently verifies owned QHS
completion, stock signed DSP/loader/GSL, all five stock inputs in the relevant
init/zygote/Bluetooth/HAL namespaces, and the full same-PID core/effect/Samsung
HAL binder set. Only then may it retire physical active/armed/mount markers and
stop their old watchdog. It attempts application finalization afterward. If
that finalizer is unavailable or still rejects ownership, the application
journal remains fenced and rollback returns failure. Later attempts only retry
metadata verification and cannot repeatedly interrupt already-normal audio.
Clearing physical arming here does not claim the application journal is
resolved or publish current Applied/Disabled proof. A failed physical proof
retains its physical markers and requires actual owned recovery.

The existing `recovery_pending` predicate combines physical pending state and
the actual parsed application fence; a directory containing only validated
terminal history does not block normal activation. Health rechecks its
generation after bounded readers, and the rollback owner repeats that check
under FD9 before any cleanup or reconciliation.

Remote uncertainty is stronger than an expired lease or changed HAL PID.
The root owner exposes read-only `latency-runtime.sh recovery-status`, scope
`additional_ttp`, with `owned_recovery_required` and `recovery_phase`. Any
present/malformed fence is unresolved until that owner's verified finalization.
A false flag alone is not a disabled receipt or stock-session proof.
`ownedRecoveryRequired` blocks new application and normal-route confirmation
until the lifecycle owner's persistent recovery journal is resolved. Its
reviewed loader/ADSP recovery and subsequent normal HAL/SPF/binder checks are
owned by the existing lifecycle path. A fresh status or normal PID alone does
not prove that remote buffers/graphs are gone. No independent GPR device FD,
DSP logging, discovery/pairing or thermal restart is introduced here.

Root/module/boot loss stops new writes and enters owned cleanup. Module removal
must order recovery before deleting its owner journals or provider artifacts.
Whole-tree death, timeout, bridge reopening, module disable and root loss must
all pass the real supervision/recovery tests before qualification. No claim is
made that this source-only preparation already passes those device tests.

## JSON and app state

All status uses `schema:1`, `scope:"latency_application"`, exact UUID/requested
pair, `current_config_matches`, typed capability flags, phase/terminal,
`last_outcome`, `retry_safe`, reason and `owned_recovery_required`. With no
operation: phase `saved_pending`, UUID null, accepted/applied/disabled false.
Save remains pending. Acceptance is only a launch acknowledgement.

Positive applied proof requires the exact new epoch, completed WR plus confirmed
rewrites, exact positive HQ delta, fresh authenticated lease and current config.
It expires within 2 seconds and on any binding mismatch. Stock HQ0 instead uses
`disabled`, `disable_receipt_valid`, `fresh`, `new_session`,
`normal_owner_verified`, `old_relay_removed`, `stock_gsl_verified` and age <2s.
It does not require `available`, current positive-profile support or a provider
lease. It does require qualified `disable_available` and current ownership.
LL nonzero remains pending in either case.

The `LatencyApplyState` UI reducer and native MainActivity/RootApi integration
are maintained separately. Receipt time is stamped using Android elapsed time
and root-session generation on receipt, never a server timestamp substituted
for the app clock. Status polling is automatic while foreground. Show current
applied/disabled separately from stored history; stale success becomes
unconfirmed. Unknown outcomes are polled under the same UUID. An explicit Apply
is enabled only by the corresponding qualified backend capability/context and
safe owner state, never by a Save or healthy worker alone.
