# QHS and Lossless

Optional startup request:

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/config.sh set classic_qhs_host_aware true'
su -c 'sh /data/adb/modules/s25_aptx_adaptive/config.sh stop'
su -c 'sh /data/adb/modules/s25_aptx_adaptive/config.sh start'
```

The trusted root-owned configuration enables one fixed Classic QHS host-aware request per observed Bluetooth/service startup. Exact controller acceptance is logged in `state/policy.log`; it is not proof of negotiated QHS. The request was accepted on-device, including after Bluetooth off/on. Public defaults remain off. Disabling this option stops future requests; it does not send an unverified inverse command. Startup timing does not guarantee the request precedes every automatic connection.

## Measurements on 2026-11-08

Samsung SM-S931Z/DZIF, Hamilton firmware `2.0.1-00353-PATCHZ-2.90463.51`; Air6 Pro firmware `v0.2.9` (standard Device Information read).

| Receiver condition | DSP QHS feedback | Encoder state |
| --- | --- | --- |
| Mac also connected; multipoint on | 0 | Ordinary HQ |
| Mac disconnected; multipoint on | 0 | Ordinary HQ |
| Mac disconnected; multipoint off; LDAC disabled | 0 | Ordinary HQ |

Controlled captures used the stock signed DSP modules. `QHS support=0` and `preferredQHS=1` were received together. The real HQ state entries were 2/3 (named ENCLEVEL 3/4); state 12, `STATE_ENCLEVEL_5_LOSSLESS`, was never observed. Lossless selection and its 16-bit display therefore did not establish actual Lossless operation in these tests.

A separate PHY-reporting request was accepted, but no matching vendor PHY-change event was observed. Event absence is not a physical-PHY measurement. The encoder received explicit sideband zero values; this was not merely an unavailable Android status.

The latest live feature query remained `1c7d7fcc31`, matching the startup cache. Firmware inspection found vendor-interface suppression conditions; their initialization and the Classic host-mode implementation delegate into ROM absent from the available patch image. No validated additional enable operation was identified. The gating conditions describe vendor-interface advertisement; they do not by themselves establish physical PHY capability. This does not prove that the radio hardware or receiver lacks QHS.

Diagnostics were stopped and the stock audio daemon and verified module restored after captures. No NVM, radio calibration or unsigned DSP changes are included. Actual QHS/Lossless activation and the 44.1kHz dropout remain unresolved.
