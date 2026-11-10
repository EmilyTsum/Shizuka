# Shizuka native settings

This companion uses [OneUIProject oneui-design](https://github.com/OneUIProject/oneui-design)
and [oneui-core](https://github.com/OneUIProject/oneui-core), rather than a WebView.
It uses the published `OneUITheme`, `ToolbarLayout`, SESL preferences, dialogs and
Material components. Versions are pinned in `build.gradle`.

All user-facing text is English. The interface follows Android settings:
a connection overview opens playback, additional delay, DSP status, diagnostics
and module pages. The app supports system dark mode, scalable system text and
window insets. Its normal launcher icon opens the app and requests the user's
standard KernelSU root permission to connect to the installed module. Optional
Action and module-settings entries remain compatible.

This APK remains owned by the KernelSU module; module uninstall removes it.
The app has no boot receiver. A fixed, bounded settings-bootstrap.sh command
checks its own UID and installed APK identity and starts only the module's UI
bridge. It never activates audio or changes settings on launch. Missing,
disabled or mismatched modules and denied root requests show English guidance
and an explicit Retry connection, without presenting cached data as current.

## Build

Use JDK 17 and an Android SDK containing platform 35 and build-tools 35.0.0:

```sh
JAVA_HOME=/path/to/jdk17 ANDROID_SDK_ROOT=/path/to/sdk python3 tools/build_settings_app.py
```

`build/Shizuka-settings.apk` is signed with a persistent local key at
`build/settings-signing/shizuka-settings.jks`. Keep that key to install future
versions over an existing local build. Set `SHIZUKA_SETTINGS_KEYSTORE` to choose a
persistent key location. This is a local-build identity, not a release authority.
`build/settings-app-build.json` binds the APK to its source/resource fingerprint.

## API boundaries

- `status.sh`: Android route/codec and fresh DSP observations.
- `playback.sh get active` / `select`: selectable Android codec preferences.
- `config.sh tuning capabilities|get|set|availability`: typed delay requests and
  firmware application status. Saving a value never claims that DSP applied it.
- `diagnostics.sh capture|list|export`: bounded opt-in captures. ZIP files are
  copied to a user-selected Storage Access Framework document.
- `control.sh status|rollback` and `apply.sh`: explicit module maintenance only.

Every root operation runs off the UI thread. Normal launch invokes only the
fixed /system/bin/su command for the app's own numeric UID and requires normal
KernelSU Superuser approval. It never edits that policy. The privileged command
has a 15-second timeout; the app bounds the full connection attempt including
the prompt to 45 seconds and 16 KiB of output. Optional Action supplies the
same temporary random abstract LocalSocket through shizuka_socket.
The client requires server peer UID 0; the bridge authenticates the companion
UID and allowlists scripts/arguments. Each request/response has a bounded
length-prefixed JSON frame; archive responses stream a bounded binary body.
Expired or missing sessions show an unavailable screen with an explicit
connection retry. The exported launcher activity accepts no command or path
from an intent. A normal app connection uses the single fixed bootstrap;
provided socket names are strictly checked and the peer must be root. Opening
the activity or accepting a socket never applies a setting or starts audio.
The API scripts are allowlisted on both sides of the bridge. Capture/session
IDs and binary archive paths are validated before use.

## Third-party notices

OneUIProject's MIT license is in `ONEUIPROJECT-LICENSE.txt` and is included in the
APK at `assets/oneui-project-license.txt`. Its SESL core modules are derived from
AndroidX and Material Components; their corresponding license notices are bundled
in the published dependencies. `gradlew` and `gradle-wrapper.jar` are Gradle's
Apache-2.0-licensed wrapper, pinned to Gradle 8.10.2.
