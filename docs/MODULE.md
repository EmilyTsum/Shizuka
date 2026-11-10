# KernelSU module lifecycle

The module retains the exact DZIF stock and payload hash checks, late runtime mounts, and audio-proof gates. A rebuilt archive starts as an unverified trial. Host lifecycle tests do not establish phone compatibility, successful reboot reapplication, or working audio.

| Entry point | Role |
| --- | --- |
| `service.sh`, `late-load.sh` | Launch the boot worker, which waits for a stable host framework and requires an exact verified payload proof. |
| `webroot/index.html`, `webroot/launcher.js` | KernelSU settings entry: one fixed handoff to the native app; no web settings or audio mutation. |
| `action.sh`, `settings.sh` | Verify/install the bundled companion APK and open settings; no audio mutation. |
| `apply.sh` | Explicitly apply or repair the module; an unverified payload gets a 900-second trial. |
| `latency-runtime.sh request` | Verify the optional runtime/native/HAL identity and prepare a current-boot relay request; no mount or process restart. |
| `control.sh` | Validate payloads, activate, keep a verified session, or restore stock state. |
| `watchdog.sh` | Request recovery when the trial expires or its Bluetooth process exits. |
| `health.sh` | Watch a kept Bluetooth process and request recovery if its identity changes. |
| `lib/runtime.sh` | Shared host-process discovery, process identity, live payload checks, tokens, and lifecycle locking. |
| `lib/properties.sh` | Generic property snapshots, write ownership and restoration rules. |
| `lib/config.sh` | Optional extension configuration and legacy-location lookup. |
| `lib/policy.sh` | Serialized QHS worker start/stop, ownership, readiness and bounded termination. |
| `lib/device.sh`, `profiles/` | Validated bundled backend selection and device-specific operations. |

## Serialized recovery

Mutating control commands hold `state/lifecycle.lock` on FD 9. Activation, keeping a session, and recovery cannot modify mounts, properties, or recovery markers concurrently. Status and preflight remain read-only. Recovery waits for an in-progress mutation to finish; it does not interrupt activation halfway through a mount operation.

Policy start/stop commands also acquire the lifecycle lock before `state/policy.lock` on FD 6. Recovery and keeping a session call the shared worker functions under their existing lifecycle lock, avoiding recursive lock acquisition in a child. Manual startup waits for recovery to finish and then rechecks whether the module remains active. All flock-owning entry points run under KernelSU BusyBox ash.

Detached watchdog and health workers close FD 9; the policy worker closes both FD 6 and FD 9. Otherwise a worker could retain a lock after its parent returns. The existing rollback lock on FD 7 and QHS ownership lock on FD 8 remain in place.

Workers call `rollback-trial TOKEN` or `rollback-health TOKEN`. The controller checks the token after acquiring the lifecycle lock. A delayed request from an old generation cannot restore stock over a newer session, and a disarmed trial cannot undo a kept session. Only a current health recovery sets `.no-auto`. Manual `control.sh rollback` remains unconditional.

New activations save both PID and `/proc/PID/stat` start time for Bluetooth and the audio HAL. The shared verifier checks these identities and all three live APK/native/HAL hashes before a session is kept. The watchdog also checks process start time, so PID reuse does not hide the original process exiting. Older PID-only records retain the previous liveness check until the next activation writes complete records.

Active/trial markers, optional runtime ownership, its remote recovery journal, a nonempty mount journal, and pending QHS restoration all count as recovery state. Explicit apply and boot activation restore this state before starting another generation. Direct activation refuses to overwrite it, including when an interrupted recovery has already removed the active/trial markers. The read-only `control.sh status` reports pending recovery instead of declaring that state inactive.

Health and policy preparation both succeed before the trial is disarmed. Policy startup is idempotent, replaces PID-only legacy watchers, and waits for the new worker to acknowledge its recorded identity and become ready. Stop confirms termination before allowing a replacement. Failures propagate to the keep/recovery command, preserving the trial arm and recovery records. A matching command with a different process identity is left untouched and reported for inspection.

## Optional latency runtime identity and request

`tools/package.py --with-latency-runtime` includes the separately verified
runtime/provider artifacts and emits `latency-runtime.identity`. Its single
ASCII line is:

```text
1 COOKIE GSL_SHA NATIVE_SHA HAL_SHA
```

The cookie and hashes come from the same source-bound runtime build and
matching native/HAL outputs. The installed identity is root-owned, singly
linked and mode 0644; the optional `latency-provider` retains executable
mode 0755. This identity is separate from the four core payload manifest
entries and cannot create or inherit an audio verification proof. Ordinary
packaging excludes stray runtime identities, provider binaries and build records.

An explicit root caller can prepare that runtime for a later module activation
in the same boot:

```sh
/data/adb/ksu/bin/busybox sh /data/adb/modules/s25_aptx_adaptive/latency-runtime.sh request
```

The command verifies the module/profile, disabled/removal markers, private
root-owned state, canonical identity, packaged GSL/JNI/HAL hashes and reviewed
GPR/OSAL dependencies. It holds the lifecycle lock itself; a caller already
holding that lock must avoid recursive acquisition. An unresolved latency
activation or another current-boot request is refused. Repeating the same valid
request preserves its inode and contents. A valid earlier-boot request is
archived as `latency-runtime.request.stale.OLD_BOOT_UUID.COOKIE`; an existing archive or symlink is
preserved and causes refusal instead of being overwritten.

Preparation atomically writes a root-owned mode 0600 record:

```text
BOOT_UUID COOKIE GSL_SHA relay
```

It does not mount a library, restart Bluetooth/HAL, start a publisher, change
saved delays, or mark a delay applied/heard. Later activation consumes the
request through the existing owner lifecycle and rechecks the relay identity
and native/HAL pair before mounting. Intervening payload or cookie changes are
rejected. Earlier-boot requests cannot automatically activate on a new boot.
The existing manually staged `observer` mode is a legacy root diagnostic path;
production `request` creates only `relay` records.

Live encoded transport remains under validation. A prepared request, successful
mount or accepted INIT is not completed RD/WR forwarding or receiver timing
evidence. Normal delay zero does not opt into an opaque diagnostic relay. See
the [external experiment](../experiments/latency/README.md) and
[encoder observation limits](ENCODER-OBSERVATIONS.md).

## Remote ownership recovery

An optional relay writes `state/latency-remote-uncertain` before stopping its
publisher. Removing the GSL overlay or changing the HAL PID does not retire
that journal. A fresh, revoked, original-HAL observation can prove untouched
ownership or acknowledged CLOSE/unmap cleanup. Otherwise the reviewed profile
uses the normal ADSP loader restart, observes offline then running, and waits
for a fresh normal HAL with core, effect and Bluetooth factory binders in the
same process. Missing readiness retains the records for diagnosis; recovery
does not automatically restart thermal services. A different boot permits
old-record archival only after stock DSP/GSL and full normal HAL verification,
and never causes an old PID signal or another DSP restart. The read-only
`latency-runtime.sh recovery-status` exposes the ownership fence. See the
[provider recovery contract](../experiments/latency/provider/README.md#read-only-health-and-owner-recovery).

## Property snapshots

The backend declares which technical properties it may change. Each has a saved value and an `-existed` marker. New snapshots use `state/properties-v2` and a write-intent journal; the intent and intended value are recorded before each actual write. Unchanged properties are not owned. Recovery restores only owned values that still match the module's write, preserving subsequent user/system changes. A missing/corrupt journal retains recovery state instead of silently skipping cleanup. Empty-value and originally-absent properties remain distinct.

Older snapshots retain their previous restoration rules. Property values are data and are never sourced as shell code. Default codec priorities, voice settings and btsnoop logging follow Android; backend-required codec exposure properties and explicit extension overrides are separate from per-device settings. See [SETTINGS.md](SETTINGS.md).

## Host checks

```sh
python3 tests/module_lifecycle_test.py
SHIZUKA_TEST_SHELL=bash python3 tests/module_lifecycle_test.py
# Optional host BusyBox, not the phone's ARM64 binary:
SHIZUKA_TEST_BUSYBOX=/path/to/busybox SHIZUKA_TEST_SHELL='/path/to/busybox sh' python3 tests/module_lifecycle_test.py
python3 tests/qhs_nvm_test.py
python3 tests/policy_lifecycle_test.py
python3 tests/latency_request_test.py
python3 tests/latency_remote_recovery_test.py
python3 tests/latency_profile_recovery_test.py
python3 tests/latency_artifacts_test.py
```

The lifecycle harness uses temporary state and property databases, real host file locks through Python's POSIX `fcntl` module, and fake Android boundaries. It covers queued stale recovery, detached-worker descriptors, PID reuse, live payload mismatches, recovery failure, and exact property restoration. The policy harness additionally runs real child processes to exercise concurrent start/stop, readiness failures and recovery interleaving. A real numeric `/proc` identity check is skipped on hosts that do not expose that record; synthetic process-record checks still run. The optional host BusyBox supplies its shell and other applets; the harness supplies file locking even when that BusyBox build omits `flock`.

`tools/verify_offline.py` records the lifecycle suite with the existing native, Java, and QHS checks. `tools/package.py` requires both that host evidence and a current build-bound native integration result from full verification. Host-only checks cannot create the integration result; see [PORTING.md](PORTING.md).
