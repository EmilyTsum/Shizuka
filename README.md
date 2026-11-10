# Shizuka

Bluetooth extensions with shared Android settings/status tools and explicit, verified native device backends. Codec selection, sample rate, bit depth, preferred transport and device permissions follow Android's Bluetooth state. The module no longer replays a private 48 kHz/HQ preference when devices connect.

The included native backend supports **SM-S931Z / S931ZSCU1DZIF only**, with exact stock hashes. Portable Android tools can be built without Samsung binaries; native support for another phone requires a reviewed backend. See [porting](docs/PORTING.md).

## Settings

KernelSU’s module **WebUI/settings entry opens the native Shizuka settings app**; **Action** opens the same interface as a fallback. The app uses [OneUIProject](https://github.com/OneUIProject/oneui-design) and shows Android playback selection, additional TTP requests, DSP observations, and opt-in diagnostic capture/export in English. Settings stay native: the two small WebUI entry assets only launch the internal app and contain no web settings. See [the app guide](docs/NATIVE-SETTINGS.md).

The app and external callers share one-shot Android preferences through `playback.sh`, typed tuning through `config.sh`, and `status.sh` / `diagnostics.sh`. Opening the app does not restart Bluetooth or install a DSP experiment. Module activation is an explicit operation through `apply.sh`. Optional extension settings live in `/data/adb/shizuka/config.conf`.

Lossless selection is separate from the observed encoder state and receiver bit-perfect playback. Saved DSP captures are opt-in; foreground DSP pages automatically observe eligible playback, and old captures remain history. The ordinary DZIF module saves additional TTP intent while the signed-DSP replacement path remains blocked by signature verification. The optional host-owned runtime reports its separate current status through `config.sh tuning availability`; preparation alone is not an applied delay. Existing playback modes continue to use the stock signed DSP.

The optional, offline [Adaptive HQ/Lossless and Adaptive LL TTP experiment](docs/TTP-TRIAL.md) has independent integer increments (HQ/Lossless 0..150 ms, Adaptive LL 0..100 ms; step 1). Typed controls and device/provider contexts share one definition catalog; exact-image DSP implementations are separate backends. The included implementation targets the verified DZIF image. It is excluded from normal module payloads and has no automatic device activation; the device loader rejected the modified candidates before execution. Offline instruction results do not establish runtime support.

The separate [external encoded-TTP experiment](experiments/latency/README.md) retains the signed DSP and implements an APSS packet transform with runtime values, including reading the UI's configuration file. Its native file transform has been checked on the phone; the HAL-owned live transport is implemented but remains under validation, and receiver timing is unverified. It is excluded from ordinary module packages and can be bundled explicitly as described below. Standalone GPR diagnostics are prohibited on this firmware because closing that device can terminate existing audio graphs; the experiment uses the existing HAL owner instead.

## Build

Requirements: Python 3.10+, a host C++17 compiler, JDK 17+ with JNI headers, Android SDK platform 35/build-tools 35.0.0, and NDK 26.1.10909125. The Samsung APK backend also requires apktool 3.0.3. The companion app uses Gradle 8.10.2, AGP 8.7.3, and pinned OneUIProject Maven dependencies (JDK 17).

Place unmodified stock files listed in [the device profile](profiles/samsung-sm-s931z-dzif/profile.json) in `inputs/`. Proprietary APKs/libraries are excluded from this repository.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
# Set JAVA_HOME, ANDROID_SDK_ROOT and SHIZUKA_NDK_BIN.
# SHIZUKA_NDK_BIN is the NDK toolchains/llvm/prebuilt/<host>/bin directory.
python tools/build_all.py
```

Output: `dist/Shizuka-samsung-sm-s931z-dzif-0.11.0-test.zip`. Install through KernelSU, then open the module’s WebUI/settings entry. Action remains available as a fallback. Explicit activation of unverified native payloads starts a 15-minute trial with automatic rollback. Manual restoration:

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
```

The installed module ID remains `s25_aptx_adaptive` for upgrade compatibility.

To build **only portable Android settings/status tools**, without stock files, apktool, NDK or a native device profile:

```sh
python tools/build_android.py --helpers-only
```

This produces `build/android-tools/`, a command-line tool bundle for Android 14+ with KernelSU root. It is not an installation ZIP. It provides `config.sh device active get` and `status.sh`; vendor controller extensions require a verified native backend.

Build selection uses `SHIZUKA_PROFILE`, `SHIZUKA_INPUTS` and `SHIZUKA_NDK_BIN`. Existing `S25_BASE_BIN`, `S25_NDK_BIN`, `S25_BUILD_TOOLS`, `S25_RATE`, `S25_MODE` and `S25_R2` variables remain accepted as compatibility aliases. New `SHIZUKA_*` variables take precedence. Details: [PORTING.md](docs/PORTING.md).

To build and include the optional latency runtime after the matching native/HAL
build, supply the reviewed stock `libar-gsl.so`, with `libar-gpr.so` and
`liblx-osal.so` alongside it:

```sh
python tools/build_latency_runtime.py --gsl /path/to/reviewed/libar-gsl.so
python tools/package.py --with-latency-runtime
```

`SHIZUKA_GSL_INPUT` can replace `--gsl`; `--gpr`/`--osal` select dependencies
stored elsewhere. The builder checks all three firmware hashes, the embedded
HAL context, source fingerprints and the matching core outputs. The separate
`build/latency-runtime-build.json` records their deterministic cookie and output
hashes. The opt-in ZIP has a `-latency` suffix. Including these files does not
activate a graph or create an audio verification proof; the four core payload
manifest and proof hashes retain their existing meaning. Ordinary packaging
excludes stray runtime/provider files.

The optional package also contains `latency-runtime.identity`, binding its
cookie and GSL output to the matching JNI/HAL pair. The root-only
`latency-runtime.sh request` command verifies those installed files and prepares
a current-boot request for later controlled activation; it does not mount,
restart audio or report an applied delay. See
[the runtime lifecycle guide](docs/MODULE.md#optional-latency-runtime-identity-and-request).

## Checks

```sh
python tools/verify_policies.py --android-jar "$ANDROID_SDK_ROOT/platforms/android-35/android.jar"
python tools/verify_offline.py --host-only
python tests/module_lifecycle_test.py
python tests/device_profiles_test.py
```

The full offline command also rebuilds native artifacts and runs the ARM64 dispatch emulator. Packaging requires its build-bound integration result as well as host evidence; `--host-only` alone cannot make a native module packageable. Failed rebuild/verification attempts invalidate previous success records. Packaging also rejects stale source/test evidence, mismatched profiles, changed outputs, and inherited runtime state/audio proofs. Host checks do not establish device/reboot/audio correctness. See [MODULE.md](docs/MODULE.md) for recovery behavior.

GPL-2.0-only; this covers repository code and modifications, not proprietary stock inputs.
