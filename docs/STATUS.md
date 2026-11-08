# Status

- Based on 0.6.0-test, with source formatting and build instructions prepared for publication.
- aptX HD: fixed the LL offload trampoline replaying `cmp w8,#3` instead of the stock `#2`, which incorrectly routed HD into the unknown-codec branch and sent an empty controller START. The corrected build passes 40 stock/private ARM64 dispatch cases; BT-B10 now receives a valid HD START with controller status 0. BT-B10 HD indication and normal audio were confirmed by listening at 48 kHz/24-bit.
- BT-B10 also rejected some aptX-to-HD switches with AVDTP BAD_STATE while the old media L2CAP channel was still closing. Genuine HD SetConfiguration now uses the existing bounded teardown guard; private Adaptive proxy and unrelated codec paths are excluded. BT-B10 aptX→HD and HD→Adaptive→HD selections passed on-device; final HD audio and switching were confirmed by the user.
- aptX LL: playback, codec round trips, and 44.1↔48 kHz switching were listening-tested with Motion+. Absolute latency has not been measured.
- Adaptive: Air6 Pro playback at 48 kHz/HQ had no observed dropouts during listening tests. Dropouts at 44.1 kHz remain unresolved.
- LE Audio: connection, display, and playback in both Air6 Pro earbuds were verified.
- QHS: optional trusted startup requests now validate exact controller acceptance, including after Bluetooth off/on. Controlled Air6 Pro measurements still reported QHS0 and ordinary HQ, including with multipoint off. Actual QHS/Lossless activation remains unresolved; see [QHS.md](QHS.md).
- Lossless: a separate selection and 44.1 kHz/16-bit display are implemented. Required 24-bit DSP input is retained. Actual lossless transmission is unverified.
- Logs showed ENCLEVEL 3↔4 transitions, but their causal relationship to dropouts is unconfirmed. Changing the resynchronization timer did not help.
- A physical click marker also occurred after about 46 seconds at unchanged ENCLEVEL 4, with consecutive encoder packet numbers around the marker. Level changes are not necessary for every reported click; encoded content, downstream timing and radio delivery are still unverified.
- The KernelSU WebUI shows Android's active media route and codec selection, independently of DSP observations. It does not enable DSP logging.
- Status snapshots were checked on-device for Adaptive 44.1 kHz/24-bit, Lossless selection 44.1 kHz/16-bit, and LE Audio LC3 48 kHz/16-bit. WebUI rendering and unavailable/stale-state behavior have host checks. See [44KHZ.md](44KHZ.md) for the investigation's measurements and limits.
- Ineffective experimental DSP timer, PCM width, and transport clock changes are excluded.
- The reconfiguration repair was applied with exact APK/native/HAL hash checks. Its initial generation passed Adaptive96/HQ and 48/HQ after 44.1→48 and identical48 reapplication with Air6 Pro. The final HQ-default generation passed a fresh Adaptive48/24-bit/HQ listening check;96, LL, LE and full reboot were not retested on that final generation. Explicit runtime LL and build overrides remain available.
- Explicit Adaptive/Lossless retries now renew the private configuration revision, including unchanged requested values; default/startup vectors remain idempotent. Lossless also bypasses Java's unchanged-feeding early return. Controller START/STOP ownership and pending callbacks survive cancellation and failed STOP. These repairs are not a verified fix for the 44.1 kHz click or every intermittent silent change.
- Android's A2DP playback flag can clear after reconfiguration while the HAL still feeds audio and the user hears normal playback. It must not trigger automatic recovery or be presented as proof of silence.

Reports should include the commit, phone/firmware, receiver, codec/sample rate, and symptoms. Remove device identifiers before publishing raw logs.
