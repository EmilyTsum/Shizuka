# SPDX-License-Identifier: GPL-2.0-only
# Profile variables form the interface consumed by control.sh and qhs-nvm.sh.
# shellcheck disable=SC2034
# Tested backend: SM-S931Z / S931ZSCU1DZIF. Other devices need their own backend.
NATIVE=/apex/com.android.bt/lib64/libbluetooth_jni.so
HAL=/vendor/lib64/libsehbluetooth_audio_session_aidl.so
APK=/apex/com.android.bt/app/Bluetooth@CP2A.260605.016/Bluetooth.apk
OFFLOAD=/vendor/lib64/btaudio_offload_if.so
FRAMEWORK=/apex/com.android.bt/javalib/framework-bluetooth.jar
NATIVE_PAYLOAD=patched-libbluetooth_jni.so
HAL_PAYLOAD=patched-libsehbluetooth_audio_session_aidl.so
APK_PAYLOAD=patched-Bluetooth.apk
STOCK_TARGET_COUNT=5
AUDIO_HAL_SERVICE=vendor.audio-hal-aidl
AUDIO_HAL_FACTORY=vendor.samsung.hardware.bluetooth.audio.ISehBluetoothAudioProviderFactory/default
NATIVE_LABEL=u:object_r:system_lib_file:s0
HAL_LABEL=u:object_r:vendor_file:s0
APK_LABEL=u:object_r:system_file:s0
QHS_ENABLED_FEATURES=1c7d7ffc31
QHS_SUPPORTED=true
QHS_DISABLED_FEATURES=1c7d7fcc31
QHS_SOC=hamilton
QHS_NVM_PATH=/vendor/firmware/hmtnv20.b39
QHS_FIRMWARE_PATH=/vendor/firmware/hmtbtfw20.tlv
QHS_NVM_ORIGINAL=16c68fb73417b926ad40c2a42a7e3ff9be78db45aa81fddb31fcb069991fb0a6
QHS_NVM_PATCHED=4aee484bfa15d22918053caaa13241f3a39463493de26b28779b70e84c7d4028
QHS_FIRMWARE_SHA=ff39d6a312648e64f81c933f324c2858c6bb1ee93a71d5440604e2230379fb37
QHS_HAL_PID_PROPERTY=init.svc_debug_pid.vendor.bluetooth-aidl-qti

profile_target_allowed() {
 case "$1" in "$NATIVE"|"$HAL"|"$APK"|"$OFFLOAD"|"$FRAMEWORK") return 0;;*) return 1;;esac
}
profile_payload_allowed() {
 case "$1" in "$NATIVE_PAYLOAD"|"$HAL_PAYLOAD"|"$APK_PAYLOAD"|runtime.prop) return 0;;*) return 1;;esac
}
profile_managed_properties() {
 cat <<'PROPERTIES'
r21 persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support present
dual-mode persist.bluetooth.enable_dual_mode_audio tracked
samsung-cap persist.bluetooth.samsung.a2dp_offload.cap present
snoop persist.bluetooth.btsnooplogmode present
aptx-voice bluetooth.hfp.codec_aptx_voice.enabled tracked
ad-priority bluetooth.a2dp.source.aptx_adaptive_priority.config tracked
hd-priority bluetooth.a2dp.source.aptx_hd_priority.config tracked
PROPERTIES
}
profile_apply_properties() {
 capability=$(cat "$STATE/samsung-cap") || return 1
 case "-$capability-" in *-aptx_hd-*) ;;*) capability="$capability-aptx_hd";;esac
 set_managed_property samsung-cap "$capability" || return 1
 # Android's codec properties win unless an extension override was explicitly selected.
 for setting in adaptive_priority aptx_hd_priority aptx_voice_enabled;do
  value=$(extension_value "$setting") || return 1
  [ "$value" != system ] || continue
  case "$setting" in adaptive_priority) saved=ad-priority;;aptx_hd_priority) saved=hd-priority;;aptx_voice_enabled) saved=aptx-voice;;esac
  set_managed_property "$saved" "$value" || return 1
 done
 case "$(cat "$MODDIR/payload/runtime.prop")" in
  persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support=true) r21_value=true;;
  persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support=false) r21_value=false;;
  *) fail 'unexpected runtime property payload';;
 esac
 set_managed_property r21 "$r21_value" || return 1
 if [ -f "$MODDIR/.enable-dual-mode" ];then set_managed_property dual-mode true;fi
}
profile_prepare_qhs_candidate() {
 printf '\003' | "$BB" dd of="$1" bs=1 seek=3817 conv=notrunc 2>/dev/null || return 1
 printf '\001' | "$BB" dd of="$1" bs=1 seek=3953 conv=notrunc 2>/dev/null
}
profile_verify_controller_firmware() {
 [ "$(getprop persist.vendor.qcom.bluetooth.soc)" = "$QHS_SOC" ] || return 1
 actual=$("$BB" sha256sum "$QHS_FIRMWARE_PATH") || return 1
 [ "${actual%% *}" = "$QHS_FIRMWARE_SHA" ]
}
profile_hal_ready() {
 [ -n "$1" ] && [ "$1" != "$2" ] || return 1
 grep -Fq "${HAL##*/}" "/proc/$1/maps" 2>/dev/null || return 1
 "$BB" timeout 2 service call "$AUDIO_HAL_FACTORY" 1599098439 2>/dev/null | grep -q 'Result: Parcel'
}
