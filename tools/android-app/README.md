# Shizuka native settings

This companion uses [OneUIProject oneui-design](https://github.com/OneUIProject/oneui-design)
and [oneui-core](https://github.com/OneUIProject/oneui-core), rather than a WebView.
It uses the published `OneUITheme`, `ToolbarLayout`, SESL preferences, dialogs and
Material components. Versions are pinned in `build.gradle`.

All user-facing text is English. The interface follows Android settings:
a connection overview opens playback, additional delay, DSP status, diagnostics
and module pages. The app supports system dark mode, scalable system text and
window insets. It connects only to the temporary module-owned root session opened by KernelSU Action.

This APK is the KernelSU module's internal native interface. It has no launcher
intent, no app-drawer entry and no independent recent-apps entry. KernelSU's
module Action installs/opens the explicit activity; module uninstall removes it.
When the module or root access is unavailable, the interface shows a blocking
unavailable screen instead of standalone settings or cached observations.

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

Every operation runs off the UI thread. The app does not invoke su or require
an independent KernelSU Superuser grant. Action supplies a temporary random
abstract LocalSocket session through the shizuka_socket intent extra.
The client requires server peer UID 0; the bridge authenticates the companion
UID and allowlists scripts/arguments. Each request/response has a bounded
length-prefixed JSON frame; archive responses stream a bounded binary body.
Expired or missing sessions show an unavailable screen and require reopening
KernelSU Action. No root command is triggered by an
untrusted intent or exported service. The non-exported activity is opened
explicitly by the root module Action; no intent extras run a command or change a setting.
The API scripts are allowlisted on both sides of the bridge. Capture/session
IDs and binary archive paths are validated before use.

## Third-party notices

OneUIProject's MIT license is in `ONEUIPROJECT-LICENSE.txt` and is included in the
APK at `assets/oneui-project-license.txt`. Its SESL core modules are derived from
AndroidX and Material Components; their corresponding license notices are bundled
in the published dependencies. `gradlew` and `gradle-wrapper.jar` are Gradle's
Apache-2.0-licensed wrapper, pinned to Gradle 8.10.2.
