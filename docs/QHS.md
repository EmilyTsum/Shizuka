# QHS and Lossless

Optional, off by default:

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/config.sh set classic_qhs_host_aware true'
```

Re-activate the module with Bluetooth ON. A trusted root-owned configuration enables two tested bits in a temporary overlay generated from the phone's exact DZIF NVM. No vendor NVM image is bundled or written to vendor storage. The existing Classic host-aware startup request is also enabled.

Activation verifies changed local controller capabilities after reset. Rollback removes only the owned overlay, restores the original input and verifies the original controller response. Re-activate after changing this option; restarting the watcher alone does not apply/remove the overlay.

Read-only capability query:

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/config.sh controller-features'
```

`local_enabled` is not negotiated QHS or Lossless. Peer support and radio conditions still determine actual operation.

## Measurements, 2026-11-08

SM-S931Z/DZIF, Hamilton2.0.1-00353, Air6 Pro v0.2.9, multipoint off and LDAC disabled:

| Controller input | DSP QHS | Actual encoder state |
| --- | --- | --- |
| Original NVM | 0 | Ordinary HQ despite Lossless selection |
| Two tested bits enabled | 1 | State12 / STATE_ENCLEVEL_5_LOSSLESS |

Controller features changed `1c7d7fcc31 → 1c7d7ffc31 → 1c7d7fcc31` when enabling/restoring the bits. A PHY notification matched Air6 Pro's Classic ACL and the reference QHS1 event prefix; its extra final byte remains uninterpreted.

The stock signed encoder reached Lossless after about 24 seconds. A steady capture contained 2,004 consecutive packet records over about 30 seconds, all at 44.1 kHz, QHS1 and Lossless state12, with no downgrade recorded. The user confirmed normal audio without dropouts in the later portion of the repeated fixed-tone test. Startup force/debug flags were zero. This verifies sender operation, not receiver PCM bit-for-bit equality or long-term stability.

Required 24-bit DSP input and the 16-bit Android Lossless display are retained. The option does not force unsupported receivers into QHS or guarantee Lossless for every recording or radio condition.

Adaptive 96 kHz/24-bit/HQ was also tested with QHS enabled. The legacy encoder accepted 96 kHz and received QHS1; quality feedback rose 3→4→5 with no recorded downward step or backlog bitrate reduction. The final continuous encoder segment was about 24 seconds; the user reported normal audio without dropouts or abnormal noise. This remains a short test, not a long-term radio delivery guarantee.

Later listening was reported as strongly stable after a possible brief initial interruption. A marked replay contained a 64.8-second continuous Lossless run; the single uncertain marker had no nearby QHS/encoder-state change. A periodic fault has not been established. Long-term and receiver-specific behavior remain under observation.
