# KernelSU module lifecycle

The module retains the exact DZIF stock and payload hash checks, late runtime mounts, and audio-proof gates. A rebuilt archive starts as an unverified trial. Host lifecycle tests do not establish phone compatibility, successful reboot reapplication, or working audio.

| Entry point | Role |
| --- | --- |
| `service.sh`, `late-load.sh` | Launch the boot worker, which waits for a stable host framework and requires an exact verified payload proof. |
| `action.sh` | Apply or repair the module; an unverified payload gets a 900-second trial. |
| `control.sh` | Validate payloads, activate, keep a verified session, or restore stock state. |
| `watchdog.sh` | Request recovery when the trial expires or its Bluetooth process exits. |
| `health.sh` | Watch a kept Bluetooth process and request recovery if its identity changes. |
| `lib/runtime.sh` | Shared host-process discovery, process identity, live payload checks, tokens, and lifecycle locking. |
| `lib/properties.sh` | Generic property snapshots, write ownership and restoration rules. |
| `lib/config.sh` | Optional extension configuration and legacy-location lookup. |
| `lib/device.sh`, `profiles/` | Validated bundled backend selection and device-specific operations. |

## Serialized recovery

Mutating control commands hold `state/lifecycle.lock` on FD 9. Activation, keeping a session, and recovery cannot modify mounts, properties, or recovery markers concurrently. Status and preflight remain read-only. Recovery waits for an in-progress mutation to finish; it does not interrupt activation halfway through a mount operation.

Detached watchdog, health, and policy workers close FD 9 before starting. Otherwise a worker could retain the lifecycle lock after its parent returns. The existing rollback lock on FD 7 and QHS ownership lock on FD 8 remain in place.

Workers call `rollback-trial TOKEN` or `rollback-health TOKEN`. The controller checks the token after acquiring the lifecycle lock. A delayed request from an old generation cannot restore stock over a newer session, and a disarmed trial cannot undo a kept session. Only a current health recovery sets `.no-auto`. Manual `control.sh rollback` remains unconditional.

New activations save both PID and `/proc/PID/stat` start time for Bluetooth and the audio HAL. The shared verifier checks these identities and all three live APK/native/HAL hashes before a session is kept. The watchdog also checks process start time, so PID reuse does not hide the original process exiting. Older PID-only records retain the previous liveness check until the next activation writes complete records.

Active/trial markers, a nonempty mount journal, and pending QHS restoration all count as recovery state. Action and boot activation restore this state before starting another generation. Direct activation refuses to overwrite it, including when an interrupted recovery has already removed the active/trial markers. The read-only `control.sh status` reports pending recovery instead of declaring that state inactive.

The health watcher starts before the trial is disarmed. If reading the process identity or preparing the watcher fails, the rollback arm remains present.

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
node tests/status_webui_test.cjs
```

The lifecycle harness uses temporary state and property databases, real host file locks through Python's POSIX `fcntl` module, and fake Android boundaries. It covers queued stale recovery, detached-worker descriptors, PID reuse, live payload mismatches, recovery failure, and exact property restoration. A real numeric `/proc` identity check is skipped on hosts that do not expose that record; synthetic process-record checks still run. The optional host BusyBox supplies its shell and other applets; the harness supplies file locking even when that BusyBox build omits `flock`.

`tools/verify_offline.py` records the lifecycle suite with the existing native, Java, and QHS checks. `tools/package.py` requires this result before packaging a new archive.
