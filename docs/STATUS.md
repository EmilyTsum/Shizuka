# Status

- Based on 0.6.0-test, with source formatting and build instructions prepared for publication.
- aptX LL: playback, codec round trips, and 44.1↔48 kHz switching were listening-tested with Motion+. Absolute latency has not been measured.
- Adaptive: Air6 Pro playback at 48 kHz/HQ had no observed dropouts during listening tests. Dropouts at 44.1 kHz remain unresolved.
- LE Audio: connection, display, and playback in both Air6 Pro earbuds were verified.
- Lossless: a separate selection and 44.1 kHz/16-bit display are implemented. Required 24-bit DSP input is retained. Actual lossless transmission is unverified.
- Logs showed ENCLEVEL 3↔4 transitions, but their causal relationship to dropouts is unconfirmed. Changing the resynchronization timer did not help.
- A physical click marker also occurred after about 46 seconds at unchanged ENCLEVEL 4, with consecutive encoder packet numbers around the marker. Level changes are not necessary for every reported click; encoded content, downstream timing and radio delivery are still unverified.
- The KernelSU WebUI shows Android's active media route and codec selection, independently of DSP observations. It does not enable DSP logging.
- Status snapshots were checked on-device for Adaptive 44.1 kHz/24-bit, Lossless selection 44.1 kHz/16-bit, and LE Audio LC3 48 kHz/16-bit. WebUI rendering and unavailable/stale-state behavior have host checks. See [44KHZ.md](44KHZ.md) for the investigation's measurements and limits.
- Ineffective experimental DSP timer, PCM width, and transport clock changes are excluded.
- The current reconfiguration repair was applied with exact APK/native/HAL hash checks. Adaptive 96 kHz/24-bit/HQ and 48 kHz/24-bit/HQ after 44.1→48 and identical 48 kHz reapplication were listening-tested with Air6 Pro. LL, LE Audio and full reboot were not retested on this generation.
- Explicit Adaptive/Lossless retries now renew the private configuration revision, including unchanged requested values; default/startup vectors remain idempotent. Lossless also bypasses Java's unchanged-feeding early return. Controller START/STOP ownership and pending callbacks survive cancellation and failed STOP. These repairs are not a verified fix for the 44.1 kHz click or every intermittent silent change.
- Android's A2DP playback flag can clear after reconfiguration while the HAL still feeds audio and the user hears normal playback. It must not trigger automatic recovery or be presented as proof of silence.

Reports should include the commit, phone/firmware, receiver, codec/sample rate, and symptoms. Remove device identifiers before publishing raw logs.
