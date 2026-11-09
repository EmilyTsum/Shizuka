# Native Shizuka settings

The companion package is `dev.shizuka.settings`. KernelSU Action verifies the bundled APK, registers or updates this internal UI for the active module if needed, and opens `MainActivity`. It uses OneUIProject's design and SESL libraries rather than a WebView. All application text is English, with light and dark themes.

Opening settings only reads state. Codec changes, saving additional TTP, collecting diagnostics, and module activation are separate explicit actions. Android keeps ownership of per-device codec preferences; there is no private connect-time override loop.

## Playback and tuning

The Playback page uses `playback.sh get active` to read the connected active A2DP peer's actual selectable codecs. Adaptive HQ, Adaptive LL, and dedicated Lossless selection preserve the extended codec type supplied by Android. Sample-rate and bit-depth combinations must be advertised together by that peer. A disconnected or ambiguous active device is rejected before a preference write.

Selection is a one-shot request. `submitted` and `observation_matches_request` describe Android selection only; neither is proof of DSP application, actual Lossless encoding, or receiver bit-perfect playback. The app keeps requested and observed values separate.

Additional HQ/Lossless and LL TTP values use the shared integer catalog, including range and step validation. On DZIF, `config.sh tuning availability` reports `blocked_signature_verification`: the stock firmware rejects modified DSP encoder segments. Saving a value or opening the app cannot apply it. The app does not offer a misleading DSP Apply action.

```sh
sh /data/adb/modules/s25_aptx_adaptive/playback.sh get active
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active adaptive hq 96000 24
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active adaptive ll 48000 24
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active lossless hq 44100 16
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning availability
```

## Diagnostics

`status.sh` supplies current route, Android codec selection and fresh DSP observations. DSP collection is explicitly started for a bounded interval through `diagnostics.sh capture SECONDS`. Old measurements, selection intent, encoder state, and receiver verification have distinct meanings. A rejected encoder load is reported with a structured `loader` diagnostic; it must not refresh an older running/Lossless observation.

Completed capture sessions can be listed and exported using the existing private diagnostic store. Raw logs and structured snapshots remain useful when the encoder cannot start. The app performs root I/O off the main thread; refresh does not start capture or change playback.

The UI is module-owned, with no launcher icon or boot receiver. It does not request a separate permanent root grant. KernelSU Action starts a bounded local Unix-socket session and passes its name to the UI. The root endpoint checks the connecting app UID and exposes only the existing fixed module APIs. Export is restricted to completed diagnostic ZIPs.

Temporary root is supported through the existing late-load entry point. Reboot ends the UI connection; opening Action after obtaining root starts a fresh session. The app reports an expired or unavailable session and does not attempt to obtain root itself. There is no TCP listener, analytics, or background root acquisition.

`uninstall.sh` stops the UI session and removes the internal APK. A session-local ownership watcher also handles KernelSU's removal marker before the next reboot, so temporary-root users do not have to recover root merely to remove the UI. Disabling the module closes the UI endpoint. Diagnostic data remains in the existing private store until deliberately removed.

## Build

```sh
python tools/build_settings_app.py
```

The builder emits `build/Shizuka-settings.apk` and source/output metadata for packaging. `tools/build_all.py` includes the app build, and module packaging validates the APK before bundling it as `settings.apk`. Signing material stays outside source control. OneUIProject license and dependency attributions are included with the app source.
