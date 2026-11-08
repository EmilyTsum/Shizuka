# Status

- Based on 0.6.0-test, with source formatting and build instructions prepared for publication.
- aptX LL: playback, codec round trips, and 44.1↔48 kHz switching were listening-tested with Motion+. Absolute latency has not been measured.
- Adaptive: Air6 Pro playback at 48 kHz/HQ had no observed dropouts during listening tests. Dropouts at 44.1 kHz remain unresolved.
- LE Audio: connection, display, and playback in both Air6 Pro earbuds were verified.
- Lossless: a separate selection and 44.1 kHz/16-bit display are implemented. Required 24-bit DSP input is retained. Actual lossless transmission is unverified.
- Logs showed ENCLEVEL 3↔4 transitions, but their causal relationship to dropouts is unconfirmed. Changing the resynchronization timer did not help.
- Ineffective experimental DSP timer, PCM width, and transport clock changes are excluded.
- The APK is rebuilt from your stock copy. The publication build's generated artifacts have not received a new device listening test. Full-reboot behavior is also unverified.

Reports should include the commit, phone/firmware, receiver, codec/sample rate, and symptoms. Remove device identifiers before publishing raw logs.
