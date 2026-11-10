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
 case "$1" in *[!0-9]*|0) return 1;;esac
 ready_start=$(process_start "$1") || return 1
 [ "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" = "$1" ] || return 1
 "$BB" awk -v stock="$HAL" -v owned="$MODDIR/payload/$HAL_PAYLOAD" \
  '$NF==stock || $NF==owned {found=1} END {exit !found}' "/proc/$1/maps" 2>/dev/null || return 1
 for service_name in android.hardware.audio.core.IModule/default android.hardware.audio.effect.IFactory/default "$AUDIO_HAL_FACTORY";do
  service_pid=$("$BB" timeout 1 dumpsys --pid "$service_name" 2>/dev/null) || return 1
  [ "$service_pid" = "$1" ] || return 1
  "$BB" timeout 1 service call "$service_name" 1599098439 2>/dev/null | grep -q 'Result: Parcel' || return 1
 done
 [ "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" = "$1" ] && process_matches "$1" "$ready_start"
}

# Normal ADSP loader entry reviewed in the installed module. No crash endpoint,
# firmware mutation, thermal restart or generic remoteproc selection is allowed.
profile_latency_recovery_preflight() {
 [ "$(getprop ro.build.version.incremental)" = S931ZSCU1DZIF ] || return 1
 [ "$(cat /sys/class/remoteproc/remoteproc1/name)" = 3000000.remoteproc-adsp ] || return 1
 [ -f /sys/kernel/boot_adsp/ssr ] && [ ! -L /sys/kernel/boot_adsp/ssr ] || return 1
 for recovery_pair in \
  /vendor_dlkm/lib/modules/adsp_loader_dlkm.ko:3d9708cbb7b5e5b64bbb0f4acea91a6ba4db9630d9ac8503f7dda2a546d8ec1c \
  /vendor/dsp/adsp/aptx_adaptive_enc_module.so.1:05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3 \
  /vendor/lib64/libar-gsl.so:4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc;do
  recovery_path=${recovery_pair%:*};recovery_expected=${recovery_pair##*:}
  recovery_actual=$(nsrun 1 "$BB" sha256sum "$recovery_path") || return 1
  [ "${recovery_actual%% *}" = "$recovery_expected" ] || return 1
 done
}
profile_latency_hal_identity() {
 recovery_pid=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
 if [ -z "$recovery_pid" ] && [ "$(getprop "init.svc.$AUDIO_HAL_SERVICE")" = stopped ];then
  printf '0 0\n';return 0
 fi
 recovery_ticks=$(process_start "$recovery_pid") || return 1
 printf '%s %s\n' "$recovery_pid" "$recovery_ticks"
}
profile_latency_stop_hal() {
 recovery_pid=$1;recovery_ticks=$2
 if [ "$recovery_pid" = 0 ];then
  [ -z "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" ] && \
   [ "$(getprop "init.svc.$AUDIO_HAL_SERVICE")" = stopped ] || return 1
 else
  [ "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" = "$recovery_pid" ] && \
   process_matches "$recovery_pid" "$recovery_ticks" || return 1
 fi
 setprop ctl.stop "$AUDIO_HAL_SERVICE" || return 1
 recovery_wait=0
 while [ "$recovery_wait" -lt 50 ];do
  if [ "$(getprop "init.svc.$AUDIO_HAL_SERVICE")" = stopped ] && \
   { [ "$recovery_pid" = 0 ] || ! process_matches "$recovery_pid" "$recovery_ticks"; };then return 0;fi
  sleep 0.1;recovery_wait=$((recovery_wait+1))
 done
 return 1
}
profile_latency_restart_dsp() {
 profile_latency_recovery_preflight || return 1
 printf '1' > /sys/kernel/boot_adsp/ssr
}
profile_latency_dsp_state() { cat /sys/class/remoteproc/remoteproc1/state; }
profile_latency_normal_ready() {
 normal_start=$(process_start "$1") || return 1
 [ "$(profile_latency_dsp_state)" = running ] || return 1
 profile_hal_ready "$1" "$2" || return 1
 "$BB" awk '$NF=="/vendor/lib64/libar-gsl.so" {found=1} END {exit !found}' "/proc/$1/maps" 2>/dev/null || return 1
 recovery_gsl=$(nsrun "$1" "$BB" sha256sum /vendor/lib64/libar-gsl.so) || return 1
 [ "${recovery_gsl%% *}" = 4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc ] && process_matches "$1" "$normal_start"
}

# FastRPC paths/log formats and approved DSP images are this backend's contract.
# Other device profiles supply their own preflight instead of changing diagnostics.sh.
profile_diagnostics_preflight() {
 [ "$(getprop ro.build.version.incremental)" = S931ZSCU1DZIF ] || fail 'unsupported DSP diagnostics firmware'
 [ "$1" != recover ] || return 0
  actual=$("$BB" sha256sum /vendor/dsp/adsp/aptx_adaptive_enc_module.so.1);actual=${actual%% *}
 profile_dsp_image_allowed "$actual" || fail 'unrecognized DSP image; diagnostics refused'
}

# Only an explicitly installed, owned trial creates this marker. Boot never applies a DSP image.
profile_recover_experiments() (
 if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ] || \
  [ -e "$STATE/latency-remote-uncertain" ] || [ -L "$STATE/latency-remote-uncertain" ];then
  "$BB" sh "$MODDIR/latency-runtime.sh" recover-dsp || return 1
 fi
 [ -e "$STATE/dsp-trial.active" ] || return 0
 [ ! -L "$STATE/dsp-trial.active" ] && [ "$("$BB" stat -c '%u' "$STATE/dsp-trial.active")" = 0 ] || return 1
 dsp_target=/vendor/dsp/adsp/aptx_adaptive_enc_module.so.1
 dsp_hash=$(nsrun 1 "$BB" sha256sum "$dsp_target");dsp_hash=${dsp_hash%% *}
 [ "$dsp_hash" = 05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3 ] || return 1
 dsp_old=$(getprop init.svc_debug_pid.vendor.audioadsprpcd_audiopd)
 setprop ctl.restart vendor.audioadsprpcd_audiopd || return 1
 dsp_attempt=0
 while [ "$dsp_attempt" -lt 20 ];do
  dsp_new=$(getprop init.svc_debug_pid.vendor.audioadsprpcd_audiopd)
  if [ -n "$dsp_new" ] && [ "$dsp_new" != "$dsp_old" ] && [ -e "/proc/$dsp_new/cmdline" ];then
   dsp_hash=$(nsrun "$dsp_new" "$BB" sha256sum "$dsp_target");dsp_hash=${dsp_hash%% *}
   [ "$dsp_hash" = 05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3 ] || return 1
   rm -f "$STATE/dsp-trial.active"
   return 0
  fi
  sleep 1;dsp_attempt=$((dsp_attempt+1))
 done
 return 1
)
profile_finish_runtime_recovery() {
 if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ] || \
  [ -e "$STATE/latency-remote-uncertain" ] || [ -L "$STATE/latency-remote-uncertain" ];then
  "$BB" sh "$MODDIR/latency-runtime.sh" recovered "$1"
 fi
}

profile_dsp_image_allowed() {
  case "$1" in
   05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3|ba24759a991e20b0a775b5cb51b8707155f45118ac15a7b73582736dfb858089|bfe204dd8b90f698df1c6107f5f78b300757619dad72e5033e4cf318490289a9) ;;
   ee628997be7f1b6d2b74899a25a4c9715e62dd495e87f75881337ec5aa5efaf8|df149eaacf731ad5a27b487a8aa2a27b10d99983e305a737c511e6b4369f8efc|16910bf1de410f38e84c889f6f424412be482b339509659084284bbb95fe992f|4c1fae4a0eb5bafbda0b19da5f5e5067fc3736f0b35df9d5a57ea5b9705f5dfe|4e7838e2ba87f0968432b8826469c29a9847caa2d231d69ac67487fe3c6bfb8f|11170588cfee87f800deee56789286ddeffbf9e0d597914e09e71cd434fb719f) ;;
   cdb67b33ae62e67e4d3cb160870c2c24eaa860fb750426ae8b2e41ca872f0646) ;;
   *) return 1;;
  esac
}

profile_activate_experiments() (
 if [ -e "$STATE/latency-runtime.request" ];then "$BB" sh "$MODDIR/latency-runtime.sh" mount || return 1;fi
 request="$STATE/dsp-trial.request"
 [ -e "$request" ] || return 0
 [ -f "$request" ] && [ ! -L "$request" ] && [ "$("$BB" stat -c '%u:%a' "$request")" = 0:600 ] || return 1
 IFS=' ' read -r requested_boot expected candidate extra < "$request" || return 1
 if [ "$requested_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
  mv "$request" "$request.stale"
  rm -f "$STATE/experiment-unverified"
  return 0
 fi
 # This exact firmware rejects modified encoder segments before execution.
 # Keep offline candidate generation separate from an accepted runtime provider.
 echo 'Shizuka: DSP tuning is unavailable: encoder signature verification rejects modified images.' >&2
 return 1
)

profile_start_runtime() {
 [ -e "$STATE/latency-runtime.active" ] || return 0
 "$BB" sh "$MODDIR/latency-runtime.sh" start
}
profile_stop_runtime() {
 if [ -e "$STATE/latency-runtime.active" ] || [ -e "$STATE/latency-provider.pid" ];then
  "$BB" sh "$MODDIR/latency-runtime.sh" stop
 fi
 return 0
}
profile_verify_runtime() {
 [ -e "$STATE/latency-runtime.active" ] || return 0
 "$BB" sh "$MODDIR/latency-runtime.sh" verify "$1"
}

profile_tuning_availability() {
 if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ];then
  recovery_boot=''
  IFS=' ' read -r recovery_boot recovery_rest < "$STATE/latency-runtime.active" || recovery_boot=''
  if [ "$recovery_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
   "$BB" sh "$MODDIR/latency-runtime.sh" recovery-status
   return $?
  fi
 fi
 if [ -e "$STATE/latency-remote-uncertain" ] || [ -L "$STATE/latency-remote-uncertain" ];then
  "$BB" sh "$MODDIR/latency-runtime.sh" recovery-status
  return $?
 fi
 if [ -x "$MODDIR/latency-provider" ] && [ -f "$MODDIR/latency-provider.sha256" ] &&
    [ ! -L "$MODDIR/latency-provider" ] && [ ! -L "$MODDIR/latency-provider.sha256" ];then
  expected=$(cat "$MODDIR/latency-provider.sha256")
  actual=$("$BB" sha256sum "$MODDIR/latency-provider");actual=${actual%% *}
  if [ "$actual" = "$expected" ];then
   "$MODDIR/latency-provider" --status --module-dir "$MODDIR"
   return $?
  fi
 fi
 cat <<'JSON'
{"schema":1,"available":false,"applied":false,"runtime_apply_supported":false,"status":"runtime_not_packaged","reason":"The optional live transport provider is missing or failed artifact verification. Saved additional delay remains pending. Signed DSP firmware is unchanged.","scope":"additional_ttp","stock_dsp_sha256":"05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3","codec_selection_available":true}
JSON
}
