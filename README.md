# Shizuka

Bluetooth extensions with shared Android settings/status tools and explicit, verified native device backends. Codec selection, sample rate, bit depth, preferred transport and device permissions follow Android's Bluetooth state. The module no longer replays a private 48 kHz/HQ preference when devices connect.

The included native backend supports **SM-S931Z / S931ZSCU1DZIF only**, with exact stock hashes. Portable Android tools can be built without Samsung binaries; native support for another phone requires a reviewed backend. See [porting](docs/PORTING.md).

## Settings

Use Android Bluetooth Settings and Developer Options for codec/device preferences. Optional extension settings live in `/data/adb/shizuka/config.conf`; no file is required for defaults. QHS and technical property overrides are described in [SETTINGS.md](docs/SETTINGS.md). Legacy configuration is read for extension controls, while old per-device preferences are ignored.

The KernelSU WebUI shows the current media route and Android codec selection. Lossless selection does not confirm actual lossless transmission. Historical measurements and known issues are in [STATUS.md](docs/STATUS.md) and [QHS.md](docs/QHS.md).

## Build

Requirements: Python 3.10+, a host C++17 compiler, JDK 17+ with JNI headers, Android SDK platform 35/build-tools 35.0.0, and NDK 26.1.10909125. The Samsung APK backend also requires apktool 3.0.3.

Place unmodified stock files listed in [the device profile](profiles/samsung-sm-s931z-dzif/profile.json) in `inputs/`. Proprietary APKs/libraries are excluded from this repository.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
# Set JAVA_HOME, ANDROID_SDK_ROOT and SHIZUKA_NDK_BIN.
# SHIZUKA_NDK_BIN is the NDK toolchains/llvm/prebuilt/<host>/bin directory.
python tools/build_all.py
```

Output: `dist/Shizuka-samsung-sm-s931z-dzif-0.8.0-test.zip`. Install through KernelSU, then use Action. New archives have no inherited audio proof and start as a 15-minute trial with automatic rollback. Manual restoration:

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

## Checks

```sh
python tools/verify_policies.py --android-jar "$ANDROID_SDK_ROOT/platforms/android-35/android.jar"
python tools/verify_offline.py --host-only
python tests/module_lifecycle_test.py
python tests/device_profiles_test.py
node tests/status_webui_test.cjs
```

The full offline command also rebuilds native artifacts and runs the ARM64 dispatch emulator. Packaging requires its build-bound integration result as well as host evidence; `--host-only` alone cannot make a native module packageable. Failed rebuild/verification attempts invalidate previous success records. Packaging also rejects stale source/test evidence, mismatched profiles, changed outputs, and inherited runtime state/audio proofs. Host checks do not establish device/reboot/audio correctness. See [MODULE.md](docs/MODULE.md) for recovery behavior.

GPL-2.0-only; this covers repository code and modifications, not proprietary stock inputs.
