# Shizuka

Experimental Bluetooth extensions for Galaxy S25: aptX Adaptive/LL integration, a separate Lossless selection, and LE Audio connection and display fixes.

**SM-S931Z / S931ZSCU1DZIF only.** Stock input hashes must match. Periodic dropouts at 44.1 kHz remain unresolved. Selecting Lossless does not confirm actual lossless transmission.

## Build

Proprietary APKs and libraries are not included. Place unmodified copies of the five stock files listed in `profile.json` in `inputs/`.

Requirements: Python 3, host clang++, JDK 17 or newer, Android SDK (platform 35 / build-tools 35.0.0), NDK 26.1.10909125, and apktool 3.0.3.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
# Set JAVA_HOME, ANDROID_SDK_ROOT and S25_NDK_BIN.
# S25_NDK_BIN points to the NDK's toolchains/llvm/prebuilt/<host>/bin.
python tools/build_all.py
```

Output: `dist/Shizuka-DZIF-0.6.0-test.zip`. Install through KernelSU, then use the module's Action button to apply it. New builds have no audio verification proof and run as a 15-minute trial with automatic rollback. To restore manually:

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
```

Host tests only: `python tools/verify_offline.py --host-only` (`JAVA_HOME` required). A full run also builds the native artifacts and executes the actual ARM64 HAL/offload dispatch branches with Unicorn, comparing stock codec routes and the two private codec routes. See [STATUS.md](docs/STATUS.md) for test coverage and known issues.

Codec status: open the module's **WebUI** (`<>`) in KernelSU. It shows the current media route and Android codec selection. Refresh is read-only; optional automatic refresh starts off. Lossless selection is not proof of lossless transmission.

GPL-2.0-only. The license covers this repository's code and modifications, not your proprietary stock files.

Optional Classic QHS startup requests and measured Lossless limitations: [QHS.md](docs/QHS.md). Controller acceptance is separate from actual QHS operation.
