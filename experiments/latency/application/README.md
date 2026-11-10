# Native delay-application preparation

This is isolated source for the [delay application flow](../../../docs/DELAY-APPLICATION-FLOW.md).
The default entry selects `UnqualifiedApplyAdapter`: availability is false and
Apply returns a structured rejection with saved settings pending. The concrete
resident factory and event-loop hookup are implemented behind literal false
source gates. No property, environment variable or argument can enable them.
The fixed executor and owned mutation handler exit 10 without audio calls.
The product control facade and native Apply UI are integrated separately, while
this DEX still uses the unqualified factory. No application executor has been
deployed or exercised on a phone. Host tests and a DEX build do not qualify the
actual route.

The portable sources implement exact codec cloning, a bounded transaction,
private atomic/idempotent journaling, fresh-proof status presentation and a
typed private guardian channel. Every journal mutation requires the existing
module lifecycle BSD flock. A shell guardian retains FD9 while Java closes it;
mutation requests contain only fixed numeric actions and typed values. No
arbitrary executor path, class-name argument or enabling property exists.

The `android/` sources implement actual framework calls behind that disabled
factory. They capture the unique active connected bonded peer and full actual
codec object; require a unique local MediaController/token with consistent
music state; pause/resume only that exact unchanged owner; preserve idle; and
connect/activate only the captured still-bonded peer. Codec restore requires a
verified operation-owned reset permit, advertised capability validation and
complete readback, including priority and all four signed opaque words. Missing
identity, permissions or observable intent is a failure. The resident token
stays in memory. A later process cannot deserialize it and press Play.

`ReadOnlyRootFacts` now implements the fixed installed reader behind
`FixedLifecycleOwner.Facts`. It verifies actual private config, module/payload
identity, boot/HAL/provider PID-start tuples, current root request, the raw
128-byte lease and 256-byte CRC status, current full codec/peer fingerprints and
an operation-owned receipt/capture digest. The existing typed configuration
policy is reused. Missing/malformed fields cannot become positive evidence.

`LatencyRootFactsMain` provides fixed read-only inspect/observe/disabled commands.
`ApplicationOwnerWriter` and `LatencyOwnerReceiptMain` implement typed ARM,
READY, FINISH, idle and normal-recovery receipt transitions under inherited
guardian ownership; the writer's source gate is false. A normal-route receipt
also recomputes stock DSP/GSL, live core hashes, retired old HAL, and all three
same-PID audio binder pings. No fence absence or new PID alone establishes it.
The guardian/executor never call ordinary `control.sh` and relock FD9. See
[INTEGRATION.md](INTEGRATION.md) for the fixed private handler and source gates.

`ApplyEntry` parses and authenticates the command before choosing its service
path. Preparation/status use `ReadOnlyApplyAdapter`, which has no mutation
channel or media token. A worker must own the current unbound queue and live
guardian before Android bootstrap; it rechecks them after bounded startup.
`AndroidApplicationFactory` connects that resident adapter to the existing
engine and fixed guardian channel. Completion closes local session/profile
resources before exiting `app_process`; it never quits Android's non-quittable
main Looper. Service disconnection or replacement invalidates the capture.
An unqualified stale worker leaves its queue untouched for normal recovery.

Target eligibility and observed profile are separate. A reviewed installed
device/module/native pair with a unique actually selected Adaptive HQ
44.1 kHz / 24-bit codec and matching advertised capability can be eligible for
an attempt while idle, before any encoder or graph exists. It does not acquire
an observed R3 profile. Only accepted INIT, strict actual wire framing and
completed WR_DONE evidence can prove the applied route. A changed codec has no
proven reset origin, so the reset permit returns null and stale restoration
stops. The HAL-only handler preserves the Bluetooth PID and full selection; it
does not treat a different codec as an authorized default reset.

Positive application is limited to actual reviewed **R3 HQ at 44.1 kHz**. Proof
must match the operation, snapshot token and config identity, a new bound
boot/cookie/HAL/graph epoch, a fresh authenticated lease, INIT/OPEN/START ACKs,
ACTIVE state, exact target/registration, completed WR frames and WR_DONE rewrite
confirmation. The observed primary TTP difference must equal HQ milliseconds
× 1000. Provider revision alone is not a new epoch. LL remains pending. No
codec switch to 44.1 kHz is performed to manufacture a supported context.

Zero is separate: verified removal of the owned relay and a fresh normal
stock-GSL session may report `disabled`, never `applied`. It requires no fake
zero-delta rewrite or provider lease. The current conservative receipt requires
a new HAL PID/start plus independent removal/recovery verification. A PID
change alone cannot clear persisted remote uncertainty.

`latency-apply-fence.sh check|health` protects normal activation/commit and
detects an interrupted guardian or its actual timeout child. Cleanup remains
available. Its separate `reconcile` command requires the inherited normal
rollback lock and independently verified full stock DSP/GSL/core/HAL evidence.
It archives only an exactly captured old runtime request and durably records
resolution before marking the journal recovered. An earlier boot never causes
old PID probes or signals. This metadata finalizer does not change playback,
configuration or codecs, and a missing reader or malformed record stays fenced.
The normal controller retires physical recovery markers only after independent
stock DSP/GSL/core namespace, QHS and all-three-binder verification. Application
metadata may remain fenced afterward; later rollback attempts then retry its
read-only finalizer without restarting already-normal audio again.

Run the portable logic, actual Android adapter source against synthetic service
objects, and actual launcher with synthetic boundaries and a real host flock:

```sh
python3 experiments/latency/application/test_application.py
python3 experiments/latency/application/test_android_session.py
python3 experiments/latency/application/test_android_entry.py
python3 experiments/latency/application/test_launcher.py
python3 experiments/latency/application/test_fence.py
python3 experiments/latency/application/test_owned_gate.py
python3 tests/latency_request_owned_test.py
```

Current host results: 614 portable checks, 36 Android service/codec/media-intent
checks, 44 Android bootstrap/factory checks, 8 launcher fixtures, 6 fence/lock
fixtures, 5 disabled-handler/fixed-forwarding fixtures and 5 owned-request
fixtures pass. The Android adapter
and fixed root entries also compile against API35 and D8. These are **not**
live Android service, media
permission, Bluetooth reconnect, cgroup-supervision or temporary-root-loss
proofs. Host launcher fixtures replace `/proc` identity on macOS; lock exclusion
uses a real BSD flock. Repeated Pause while already paused may be invisible to
framework observers; no stronger intent claim is made.

Build the separate preparation DEX outside the checkout:

```sh
ANDROID_SDK_ROOT=/path/to/sdk \
  python3 experiments/latency/application/build.py --output-dir /external/build
```

This emits `latency-application.dex`, SHA-256 and a source-bound build manifest.
It does not change the policy/status DEX, APK or device. No vendor binaries,
captures, private addresses, credentials or generated outputs belong here.
