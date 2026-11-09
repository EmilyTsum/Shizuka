# Settings ownership

| Preference | Source of truth | Shizuka behavior |
| --- | --- | --- |
| Codec, sample rate, bit depth, mode | Android's active codec configuration | Read the current selection; no connect-time replay. Reconnection behavior follows the OS. |
| Classic/LE output and call transport | Android preferred-audio-profile API/database | Submit an explicit change, then read Android's saved result. |
| A2DP, headset and LE connection permissions | Android profile connection policy | Submit an explicit change and verify readback. |
| HD Audio/optional-codec permission | Android's optional-codec preference | Store through the Android API, with no plugin copy. |
| QHS opt-in and technical property overrides | Minimal extension file | Applied only through a verified native backend. |

The helper never selects an active device or opens Classic in response to an LE route change. With QHS off, there is no resident Java preference policy. An old replay watcher is stopped when keeping the module; new QHS watchers have a recorded PID/start-time identity.

## System settings commands

Run the installed helper as root; the portable `build/android-tools` bundle uses the same interface.

```sh
sh /data/adb/modules/s25_aptx_adaptive/config.sh device active get
sh /data/adb/modules/s25_aptx_adaptive/config.sh device AA:BB:CC:DD:EE:FF transport output le
sh /data/adb/modules/s25_aptx_adaptive/config.sh device AA:BB:CC:DD:EE:FF transport duplex classic
sh /data/adb/modules/s25_aptx_adaptive/config.sh device AA:BB:CC:DD:EE:FF profile a2dp true
sh /data/adb/modules/s25_aptx_adaptive/config.sh device AA:BB:CC:DD:EE:FF optional-codecs true
```

`active` requires one unambiguous active device. An explicit address must already be bonded. Command/address validation happens before Android service initialization; `config.sh help` works without contacting Bluetooth or reading extension configuration. Output includes Android's saved transport, connection policies, optional-codec permission and current codec status. Missing APIs/profiles are reported per field. A write rejected or not confirmed by Android fails instead of saving a plugin fallback. Codec changes and detailed parameters remain in Android Settings/Developer Options; Shizuka adds no persistent shadow store for values the OS does not retain.

Status retains the codec name reported by Android. Private Lossless/LL IDs, Adaptive mode words and DSP session matching are interpreted only for the verified Samsung backend; an unknown codec's numeric fields are not labeled HQ or low latency by guesswork.

## Extension file

The optional canonical path is `/data/adb/shizuka/config.conf`. Defaults:

```ini
schema=2
classic_qhs_host_aware=false
aptx_voice_enabled=system
adaptive_priority=system
aptx_hd_priority=system
```

`system` leaves the property untouched. Voice accepts `true`/`false` as explicit overrides; priorities accept decimal values from 0 to 999999. These are technical properties, not a per-device codec cache. The backend still supplies properties/defaults needed to expose its added codecs. Normal btsnoop logging follows Android and is no longer enabled automatically.

`config.sh set KEY VALUE` saves only extension keys, using a temporary file and atomic rename with mode 0600. Concurrent saves and migration share a stable `.config.lock`: each writer locks, rereads the latest configuration, changes its own key and replaces the canonical file. An in-process monitor also serializes Java threads. Reading creates neither a directory nor a lock. The configuration directory must be owned by root with mode 0700, and the lock must be a root-owned regular file with mode 0600; symlinks are rejected.

QHS true is effective only for a trusted root-owned, non-writable-by-others regular file. Controller reads/requests also require a supported backend; portable tools do not enable them. After changing runtime extensions, run `control.sh rollback` and then KernelSU Action to apply a fresh native session. Action on an already active trial does not replace its properties or controller setup.

`config.sh start` and `stop` share the activation/recovery lock and a policy lock. Startup waits for the worker's PID/start-time record and readiness message; stopping waits for that exact process to exit before clearing its records. A failed or mismatched worker identity retains recovery state. The readiness message confirms the watcher is running, not that a controller command or lossless transmission succeeded.

## Existing configurations

When the canonical file is absent, the old `/data/adb/s25_bluetooth/config.conf` is read for extension controls. Schema-1 address, transport, permissions, rate/depth/mode and connect-time-apply settings are ignored. Legacy forced priorities of 4001 and voice=false become `system`; explicitly different technical overrides and the secure QHS opt-in are retained.

`config.sh migrate` writes the reduced schema-2 file to the canonical location and preserves the original file. It does not copy stale per-device choices over Android's current state. Migration and unrelated extension saves never promote an untrusted QHS opt-in; enabling it requires an explicit `set classic_qhs_host_aware true`. A malformed or symlinked canonical file fails validation rather than reviving the old configuration.

## API references

Android stores preferred audio profiles and optional-codec permissions through its Bluetooth service/database. The implementation follows the framework's [preferred-profile API](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/framework/java/android/bluetooth/BluetoothAdapter.java) and [optional-codec database preference](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/cdff4c6313bd934cb9cc0cf45986a37f627444f3/android/app/src/com/android/bluetooth/btservice/storage/DatabaseManager.java). Current codec requests follow [A2dpService](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/android/app/src/com/android/bluetooth/a2dp/A2dpService.java); detailed-parameter persistence depends on the OS implementation.
