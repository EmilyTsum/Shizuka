# Native Shizuka settings

The companion package is `dev.shizuka.settings`. Open **Shizuka** from its normal launcher icon. On first connection, allow its standard root request in KernelSU, then retry if needed. It uses OneUIProject's native design and SESL libraries. All application text is English, with light and dark themes. KernelSU Action and the optional module settings entry still open the same native activity.

Opening settings does not activate the module or restart audio. Foreground DSP pages automatically observe eligible playback as described below; saved captures and exports remain explicit actions. Codec changes, saving additional TTP and module activation are also explicit. Android keeps ownership of per-device codec preferences; there is no private connect-time override loop.

## Normal app connection

The launcher opens the native activity without requiring an Action. A missing
session starts one background connection attempt using only `/system/bin/su`
and a fixed module-owned `settings-bootstrap.sh` command. The numeric UID comes
from the app process; no intent or user-supplied path or command is accepted.
KernelSU decides whether to grant root. The app never changes its allowlist or
automatically approves a permission request.

The bootstrap verifies the enabled module, current Android user's installed
companion UID and exact module/installed APK hash pair. It starts only the
existing UI owner and bounded LocalSocket bridge; it does not install software,
activate audio, play a tone, change codec preferences or save settings. The
privileged command has a 15-second deadline; the app waits at most 45 seconds
including the root prompt, with a 16-KiB output limit. Connection errors show
English guidance, details and an explicit **Retry connection**. Polling never
repeats the root request. App and module version mismatches remain unavailable
until their matching APKs are installed.

## Optional KernelSU entry

KernelSU’s documented non-Action entry is a WebView loaded from
webroot/index.html. The reviewed upstream Manager routes that entry to
WebUIActivity; its module interface does not expose a native-activity target.
Shizuka therefore includes only index.html and launcher.js as a fixed handoff
to the native app. The former browser settings, status JavaScript and CSS stay
removed.

The handoff executes one fixed command through KernelSU’s official exec API:
the installed module’s settings.sh under KernelSU BusyBox. It does not accept
commands, addresses, URLs, query values or settings input. It attempts one
automatic launch when the page loads. Returning to the page does not launch
again; the English Open Shizuka button explicitly retries. Failure and timeout
leave Action available as a fallback. Both paths use the same APK verification,
module lifecycle and bounded native root session. Opening either entry does not
activate a module or alter audio.

Packaging allows only the exact two reviewed handoff assets by name and hash.
Additional or modified HTML, JavaScript, CSS, nested WebUI files and symlinks are
rejected. The native UI owns every playback, tuning and diagnostics screen.

Sources: [KernelSU WebUI guide](https://kernelsu.org/guide/module-webui.html),
[Manager entry routing](https://github.com/tiann/KernelSU/blob/dd1a0c0d3fe174dc3daf89b04c55579cb4acd6be/manager/app/src/main/java/me/weishu/kernelsu/ui/screen/module/ModuleScreen.kt),
[official exec bridge](https://github.com/tiann/KernelSU/blob/dd1a0c0d3fe174dc3daf89b04c55579cb4acd6be/manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewInterface.kt).

## Playback and tuning

The Playback page uses `playback.sh get active` to read the connected active A2DP peer's actual selectable codecs. Adaptive HQ, Adaptive LL, and dedicated Lossless selection preserve the extended codec type supplied by Android. Sample-rate and bit-depth combinations must be advertised together by that peer. A disconnected or ambiguous active device is rejected before a preference write.

Selection is a one-shot request. `submitted` and `observation_matches_request` describe Android selection only; neither is proof of DSP application, actual Lossless encoding, or receiver bit-perfect playback. The app keeps requested and observed values separate.

Additional HQ/Lossless and LL TTP values use the shared integer catalog, including range and step validation. The host-owned relay keeps signed DSP firmware unchanged. Without a verified optional runtime provider, `config.sh tuning availability` reports `runtime_not_packaged`; with one, it reports that provider's current, identity-checked observation. An unresolved remote recovery journal overrides the observation with `recovery_required`. Saving a value, preparing a runtime request or opening the app does not apply it. The UI distinguishes saved requests, pending activation and fresh observed packet changes. The relay remains under validation; its current strict positive-transform scope is R3 Adaptive HQ at 44.1 kHz, not general HQ96/LL/Lossless support. [Runtime request preparation](MODULE.md#optional-latency-runtime-identity-and-request) is separate from session recreation and audio verification.

```sh
sh /data/adb/modules/s25_aptx_adaptive/playback.sh get active
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active adaptive hq 96000 24
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active adaptive ll 48000 24
sh /data/adb/modules/s25_aptx_adaptive/playback.sh select active lossless hq 44100 16
sh /data/adb/modules/s25_aptx_adaptive/config.sh tuning availability
```

## Diagnostics

`status.sh` supplies current route, Android codec selection and fresh DSP observations. Opening DSP status automatically observes a single playing Adaptive or Lossless A2DP stream. The app renews a private observation token while that page, or its details page, is in the foreground. Leaving the page, pausing the app, losing the module/root authority, or detecting a route/codec/session-context change stops the current observation. This does not identify a unique radio stream or detect every track change. An unrenewed token expires after five seconds; each native reader also has a 120-second maximum lifetime. Automatic observation keeps a bounded status snapshot and encoder history, without retaining raw capture sessions.

Normal stop and independent reader/watchdog cleanup remove the run's owned log
configuration. Forcefully killing an entire process tree can leave that file
behind. The next observation or explicit capture automatically recovers only a
verified dead owner's configuration, checking its PID/start/boot identity,
owner, mode, exact contents and inode. Live, external and replaced files are
preserved and collection is refused; recovery never opens a DSP transport.

The overview shows a readable encoder state such as **Level 5 · Lossless**, a separate encoder quality or bitrate-feedback value when available, and the latest observed state change. The details page keeps the exact raw `STATE_ENCLEVEL_*` value, timing, sources, freshness, and up to eight observed state transitions. Legacy HQ/LL bitrate feedback is not converted into an R3 ENCLEVEL or a Lossless claim. Historical values include their observation time and module-session relationship, and are never marked current. A state transition alone does not prove an audio interruption or receiver bit-perfect playback. A rejected encoder load is reported independently and cannot renew an older encoder state.

When one complete R3 summary supplies a valid size, channel count and interval,
the overview also shows **Encoder bitrate (frame estimate)**. This is the
selected encoded-frame budget, not a measured radio or transmitted bitrate.
[Encoder observation notes](ENCODER-OBSERVATIONS.md) document the verified units,
channel-count factor, parser bounds and output-path limitation. DSP Details
also shows **Encoder output buffer** when an exact, fresh stock output-size
log is available. It reports one actual CAPI buffer's bytes, including its
wrapper; it is separate from the frame estimate and is not transmitted bitrate.
DSP pages poll once per second while foreground. A bitrate-only change updates
the overview even when ENCLEVEL is unchanged; unchanged visible values keep
the existing views. Freshness also expires independently after three seconds,
including time spent waiting for another API call or UI dispatch.

Explicit diagnostic captures remain available through `diagnostics.sh capture SECONDS`. Completed captures can be listed and exported using the existing private diagnostic store. Raw retention is bounded; filling its budget truncates the retained raw log while observation continues for the requested interval. The app performs root I/O off the main thread. Observation never changes playback preferences or starts audio.

The app has a normal launcher entry and no app boot receiver. It remains owned by the installed module and is removed with that module. Normal app launch uses its user-approved KernelSU root access to obtain the same bounded Unix-socket session as the optional KernelSU entries. The root endpoint checks the connecting app UID and the client checks root peer credentials; only fixed module APIs are exposed. Export is restricted to completed diagnostic ZIPs.

Temporary root is supported through the existing late-load entry point. Reboot ends the connection and temporary root must be restored first. Open the app and retry its connection; the optional module entries also remain available. A denied or unavailable root request does not show cached observations as current. There is no TCP listener, analytics or automatic root acquisition in the background.

`uninstall.sh` stops the UI session and removes the internal APK. A session-local ownership watcher also handles KernelSU's removal marker before the next reboot, so temporary-root users do not have to recover root merely to remove the UI. Disabling the module closes the UI endpoint. Diagnostic data remains in the existing private store until deliberately removed.

## Build

```sh
python tools/build_settings_app.py
```

The builder emits `build/Shizuka-settings.apk` and source/output metadata for packaging. `tools/build_all.py` includes the app build, and module packaging validates the APK before bundling it as `settings.apk`. Signing material stays outside source control. OneUIProject license and dependency attributions are included with the app source.
