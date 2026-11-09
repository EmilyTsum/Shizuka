# Device backends and portable tooling

The included native patch is verified against the SM-S931Z DZIF stock inputs. Moving offsets, changing a model label, or adding an input hash does not establish compatibility with another firmware. Portable Android settings/status helpers are independent of this backend and can be built with `tools/build_android.py --helpers-only`.

| Layer | Location | Responsibility |
| --- | --- | --- |
| Android helpers | `tools/android/` | System preferences, extension configuration, shared profile discovery/cleanup, read-only status and optional controller policy. |
| Tuning definitions | `config/tuning.json` | Integer domains, aliases and device/provider/context capabilities; generated Java definitions are checked for drift. |
| Portable tuning planner | `TuningCatalog`, `CodecTuning`, `tools/shizuka/tuning.py` | Internal and external API contracts, canonical parameters and context operations; no firmware offsets. |
| DSP implementations | `tools/shizuka/dsp_backends/` | Exact-image TTP patching, state mappings, instruction oracle and ABI verification. |
| Shared codec interfaces/logic | `src/` | Codec negotiation and bridge interfaces. Existing private ABI tags remain stable. |
| Native device implementation | `src/backends/samsung_dzif/` | Firmware-specific memory layout, JNI integration, callbacks and linker layout. |
| Native patch builder | `tools/shizuka/backends/` | Hook offsets, trampolines, binary patching, host case matrix and dispatch verification. |
| Device data | `profiles/<profile>/` | Exact stock inputs and APK patch definitions. |
| Runtime backend | `module/profiles/<profile>.sh` | Paths, payload names, SELinux labels, HAL readiness, managed properties and controller firmware conditions. |
| Shared lifecycle | `module/lib/`, `control.sh` | Serialized activation/recovery, process identities and property-write ownership. |
| Build infrastructure | `tools/shizuka/` | Validated options, profiles, fingerprints and artifact provenance. |

## Build selection

`SHIZUKA_PROFILE` selects a registered profile name or an explicit profile JSON path. The default is `samsung-sm-s931z-dzif`. `SHIZUKA_INPUTS` selects the stock-input directory; `SHIZUKA_NDK_BIN` selects the NDK LLVM bin directory. `SHIZUKA_BUILD_TOOLS` defaults to 35.0.0. `SHIZUKA_RATE`, `SHIZUKA_MODE`, and `SHIZUKA_R2` are validated backend build options, not stored per-device preferences. The old `S25_*` counterparts remain compatibility aliases; canonical values win.

The existing module ID stays `s25_aptx_adaptive` so an upgrade does not install another concurrent native module. Archive names include the selected runtime profile and module version.

`ProfileSession` owns one invocation's proxy handles and discovery state without Android dependencies. `BluetoothProfiles` adapts Android callbacks/timeouts to that session on the main event loop; both device settings and status use it. This boundary can be tested without a phone and does not add another preference store or change codec routing.

## Adding native support

1. Establish the target ABI and collect exact unmodified input hashes, runtime paths, HAL behavior and APK changes. Add a device profile containing `id`, `driver`, `runtime_profile`, stock `targets` and the four named `payloads` roles. Paths/digests/names are validated; duplicate records and traversal are rejected.
2. Implement and register the native backend in `tools/shizuka/backends/`. Its contract is `build(profile)`, `host_flags()`, `host_cases(jni)` and `verify_dispatch(profile)`, with `HOST_REQUIRES_JNI` when needed. `build` returns a dictionary of the actual validated build options; its manifest and output hashes are recorded with those options. `verify_dispatch` must raise on any binary integration failure. The existing DZIF backend deliberately pins its supported identity and offsets; another ABI needs its own implementation and regression cases. Registration is explicit Python code, not an arbitrary module import from JSON.
3. Supply the bundled runtime shell backend. It provides allowed stock/payload targets, property definitions/application, and `profile_hal_ready(new_pid, previous_pid)`. Controller support is opt-in backend capability with exact firmware/NVM conditions. A backend that needs no property overrides can return an empty managed-property table.
4. Verify unchanged stock dispatch and failure paths, then validate on the actual device, including rollback and reboot. Host success alone is not an audio proof. Unknown profiles have no fallback to the Samsung offsets.

Native/APK artifacts and test evidence carry the profile identity/fingerprint. Packaging also checks current source fingerprints and output hashes, generates the stock manifest/profile selector, and excludes runtime state, enable/disable flags and previous audio proofs. This prevents accidentally combining another profile's artifacts or treating an old device's successful listening test as current evidence.

## Verification stages

`verify_offline.py --host-only` runs the shared and backend host suites. It produces host evidence, which is insufficient by itself to package a native module. Full `verify_offline.py` also rebuilds the native artifacts and runs the backend's binary dispatch checks. Only after those checks succeed is `build/native-validation.json` published, binding the current source/profile to the exact Android/native build records. Packaging requires both kinds of evidence.

Starting a native/APK rebuild invalidates its previous build record and integration result before checking inputs. Full verification also removes previous integration evidence before running tests; failed stages never leave that old result usable. Build records and completed evidence are published by atomic rename. Changed outputs, manifests, metadata, profiles or sources invalidate their dependent result. A source change during a verification run also fails the run. None of these host or binary checks establishes phone, reboot or audible correctness.

## DSP tuning and diagnostics

DSP tuning has a separate explicit backend registry from Bluetooth/JNI patching.
A profile can expose no tuning providers, reuse a proven backend, or supply a
new one. The generic planner accepts future codec/mode identifiers and uses
provider matching to decide support; it never treats an unknown profile as a
Samsung device. Multiple controls per context and different integer domains /
units are supported. See [CONTROLS.md](CONTROLS.md) for the extension steps.

The currently included DSP implementation pins one Hexagon stock image. Its
addresses and firmware assumptions live in `samsung_dzif_ttp.py`; the actual-byte
execution oracle lives in `samsung_dzif_verify.py`. The common builder/verifier
dispatch through registered code and exact image hashes. Diagnostics similarly
asks the bundled runtime profile for `profile_diagnostics_preflight`; firmware
and approved-image checks are outside the common command wrapper. The present
native collector uses Qualcomm FastRPC, not a universal DSP memory interface.
Other acquisition ABIs need their own collector and decoder provider.

Host Linux-only suites can run in a local Linux VM on macOS. On 2026-11-09 the
new TTP candidates and their Hexagon harnesses were built on the Mac and executed
in a QEMU HVF VM on that same Mac. mn-core-01 is no longer used for this work.

Explicit device experiments may implement `profile_activate_experiments` and
`profile_recover_experiments`. The common lifecycle calls activation with radio
OFF and the watchdog already armed; recovery runs after journaled mounts are
removed and before restarting the HAL. Device code must stage only approved
images, journal before publication, retire cross-boot requests and rebuild a
stock graph on recovery. No experiment is inferred from a saved tuning value.
