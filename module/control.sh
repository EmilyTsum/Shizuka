#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
[ -x "$BB" ] || { echo 'KernelSU busybox unavailable' >&2; exit 1; }
log() { echo "s25_aptx_adaptive: $*"; }
fail() { log "$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || fail 'root required'
# Android mksh can mark numeric shell FDs close-on-exec; run every flock
# under the fixed KernelSU ash binary. Inspect executable identity, not an
# inherited environment flag, so ordinary `sh control.sh` is safe too.
bb_exe=$("$BB" readlink -f "$BB")
shell_exe=$("$BB" readlink -f "/proc/$$/exe")
[ -n "$bb_exe" ] && [ -n "$shell_exe" ] || { echo 'Cannot identify shell executable' >&2;exit 1; }
if [ "$shell_exe" != "$bb_exe" ];then
 exec "$BB" sh "$0" "$@"
fi
STATE="$MODDIR/state"
NATIVE=/apex/com.android.bt/lib64/libbluetooth_jni.so
HAL=/vendor/lib64/libsehbluetooth_audio_session_aidl.so
APK=/apex/com.android.bt/app/Bluetooth@CP2A.260605.016/Bluetooth.apk
. "$MODDIR/lib/runtime.sh"
. "$MODDIR/lib/properties.sh"
# Serialize activation, commit and recovery, including watchdog requests.
case "${1:-}" in
 activate|commit|keep-verified|rollback|rollback-trial|rollback-health) lock_lifecycle;;
esac
quiesce_radio() {
 cmd bluetooth_manager disable >/dev/null 2>&1 || return 1
 radio_wait=0
 while [ "$radio_wait" -lt 15 ];do
  radio_state=$(dumpsys bluetooth_manager | "$BB" awk '/^  State:/ {print $2}')
  case "$radio_state" in OFF|BLE_ON) return 0;;esac
  sleep 1;radio_wait=$((radio_wait+1))
 done
 return 1
}
resume_radio() {
 if [ ! -f "$STATE/radio-on" ] || [ "$(cat "$STATE/radio-on")" != 0 ];then
  cmd bluetooth_manager enable >/dev/null 2>&1 || true
  radio_wait=0
  while [ "$radio_wait" -lt 25 ];do
   radio_state=$(dumpsys bluetooth_manager | "$BB" awk '/^  State:/ {print $2}')
   [ "$radio_state" != ON ] || return 0
   sleep 1;radio_wait=$((radio_wait+1))
  done
 fi
}
start_health() {
 watched=$(cat "$STATE/bt.pid")
 if [ -f "$STATE/bt.start" ];then
  started=$(cat "$STATE/bt.start")
  process_matches "$watched" "$started" || return 1
 else
  started=$(process_start "$watched") || return 1
 fi
 health_token=$(cat /proc/sys/kernel/random/uuid)
 echo "$health_token" > "$STATE/health-token"
 "$BB" setsid "$BB" sh "$MODDIR/health.sh" "$MODDIR" "$watched" "$started" "$health_token" 9>&- > "$STATE/health.log" 2>&1 < /dev/null &
 echo $! > "$STATE/health.pid"
}
start_policy() {
 if [ -f /data/adb/s25_bluetooth/config.conf ];then
  "$BB" sh "$MODDIR/config.sh" apply-transport > "$STATE/policy-apply.log" 2>&1 || log "profile preference deferred; see state/policy-apply.log"
  "$BB" sh "$MODDIR/config.sh" start 9>&- || log "policy watcher did not start"
 fi
}
allowed() {
 case "$1" in
  "$NATIVE"|"$HAL"|/vendor/lib64/btaudio_offload_if.so|/apex/com.android.bt/app/Bluetooth@CP2A.260605.016/Bluetooth.apk|/apex/com.android.bt/javalib/framework-bluetooth.jar) return 0;;
  *) return 1;;
 esac
}
check_inputs() {
 [ -s "$MODDIR/inputs.sha256" ] || fail 'profile missing'
 count=0
 while IFS=' ' read -r digest path; do
  [ -n "$digest" ] || continue
  allowed "$path" || fail "unexpected target: $path"
  [ "$(sha "$path")" = "$digest" ] || fail "input hash mismatch: $path"
  count=$((count+1))
 done < "$MODDIR/inputs.sha256"
 [ "$count" = 5 ] || fail 'incomplete profile'
}
check_payloads() {
 [ -s "$MODDIR/payloads.sha256" ] || fail 'payload manifest missing'
 count=0
 while IFS=' ' read -r digest name; do
  case "$name" in patched-libbluetooth_jni.so|patched-libsehbluetooth_audio_session_aidl.so|patched-Bluetooth.apk|runtime.prop) ;;
   *) fail 'unexpected payload name';;
  esac
  [ "$(sha "$MODDIR/payload/$name")" = "$digest" ] || fail "payload hash mismatch: $name"
  count=$((count+1))
 done < "$MODDIR/payloads.sha256"
 [ "$count" = 4 ] || fail 'incomplete payload manifest'
}
qhs_pending() { [ -e "$STATE/qhs-nvm/intent" ] || [ -e "$STATE/qhs-nvm/restoring" ]; }
qhs_features() {
 "$BB" sh "$MODDIR/config.sh" controller-features > "$STATE/qhs-controller-features.json"
 "$BB" grep -Eq '"local_enabled"[[:space:]]*:[[:space:]]*'"$1"'([,}[:space:]])' "$STATE/qhs-controller-features.json" || fail 'controller QHS local capability readback mismatch'
 case "$1" in true) qhs_hex=1c7d7ffc31;;false) qhs_hex=1c7d7fcc31;;*) return 1;;esac
 "$BB" grep -Eq '"features_hex"[[:space:]]*:[[:space:]]*"'"$qhs_hex"'"' "$STATE/qhs-controller-features.json" || fail 'controller feature bytes differ from tested profile'

}
rollback() (
 # Never inherit activation cleanup traps into the serialized recovery shell.
 trap - EXIT HUP INT TERM
 [ -d "$STATE" ] || { log 'inactive';return; }
 [ ! -L "$STATE" ] || fail 'unsafe state symlink'
 [ ! -L "$STATE/rollback.lock" ] || fail 'unsafe rollback lock'
 exec 7> "$STATE/rollback.lock"
 "$BB" flock 7
 # Check the generation while holding the lifecycle lock. A delayed worker
 # must not undo a newer activation or a trial that has already been kept.
 case "${1:-manual}" in
  trial) token_matches "$STATE/armed" "$2" || return 0;;
  health)
   [ -f "$STATE/active" ] && token_matches "$STATE/health-token" "$2" || return 0
   touch "$MODDIR/.no-auto"
   ;;
 esac
 if [ ! -f "$STATE/active" ] && [ ! -f "$STATE/armed" ] && [ ! -s "$STATE/mounts.tsv" ] && ! qhs_pending;then check_inputs;log 'already inactive; stock hashes match';return;fi
 if [ -f "$MODDIR/config.sh" ];then "$BB" sh "$MODDIR/config.sh" stop || true;fi
 rm -f "$STATE/health-token"
 qhs_restoring=0
 if qhs_pending;then
  "$BB" sh "$MODDIR/qhs-nvm.sh" remove || return 1
  qhs_restoring=1
 fi
 quiesce_radio || true
 if [ -s "$STATE/mounts.tsv" ]; then
  "$BB" tac "$STATE/mounts.tsv" | while IFS=' ' read -r mount_pid mount_ns target; do
   [ -e "/proc/$mount_pid/ns/mnt" ] || continue
   current_ns=$("$BB" readlink "/proc/$mount_pid/ns/mnt")
   [ "$current_ns" = "$mount_ns" ] || continue
   nsrun "$mount_pid" "$BB" umount -l "$target" 2>/dev/null || true
  done
 fi
 # A Bluetooth child may have inherited the temporary zygote mount.
 current_bt=$(host_bt || true)
 if [ -n "$current_bt" ];then
  for one in $current_bt;do nsrun "$one" "$BB" umount -l "$NATIVE" 2>/dev/null || true;nsrun "$one" "$BB" umount -l "$APK" 2>/dev/null || true;done
 fi
 restore_properties || return 1
 previous_hal=$(getprop init.svc_debug_pid.vendor.audio-hal-aidl)
 setprop ctl.restart vendor.audio-hal-aidl
 hal_wait=0
 while [ "$hal_wait" -lt 20 ];do
  next_hal=$(getprop init.svc_debug_pid.vendor.audio-hal-aidl)
  if [ -n "$next_hal" ] && [ "$next_hal" != "$previous_hal" ] && grep -q libsehbluetooth_audio_session_aidl.so "/proc/$next_hal/maps" 2>/dev/null;then
   if "$BB" timeout 2 service call vendor.samsung.hardware.bluetooth.audio.ISehBluetoothAudioProviderFactory/default 1599098439 2>/dev/null | grep -q 'Result: Parcel';then break;fi
  fi
  sleep 1;hal_wait=$((hal_wait+1))
 done
 [ -z "$current_bt" ] || kill -TERM $current_bt 2>/dev/null || true
 sleep 3
 if [ "$qhs_restoring" = 1 ];then "$BB" sh "$MODDIR/qhs-nvm.sh" scan-restore;fi
 resume_radio
 if [ "$qhs_restoring" = 1 ];then
  qhs_features false
  "$BB" sh "$MODDIR/qhs-nvm.sh" finish-remove
 fi
 rm -f "$STATE/active"
 check_inputs
 : > "$STATE/mounts.tsv"
 # Keep recovery armed until owned NVM + running controller + native cleanup
 # all verify. The subshell lock serializes manual/watchdog rollback attempts.
 rm -f "$STATE/armed"
 if [ -f "$STATE/watchdog.pid" ];then
  old_watch=$(cat "$STATE/watchdog.pid")
  if [ -f "/proc/$old_watch/cmdline" ] && "$BB" tr '\000' ' ' < "/proc/$old_watch/cmdline" | grep -F "$MODDIR/watchdog.sh" >/dev/null;then kill -TERM "$old_watch" 2>/dev/null || true;fi
 fi
 log 'verified stock hashes after unmount; restored properties and restarted audio/Bluetooth'
)
case "${1:-status}" in
 status)
  if [ -f "$STATE/active" ];then
   verify_active_payloads
   log "active; exact APK/native/HAL hashes verified; Bluetooth=$live_bt HAL=$live_hal"
   if [ -f "$STATE/armed" ];then log 'trial: watchdog armed';fi
  else check_inputs;log 'stock hashes match; inactive';fi
  ;;
 preflight) check_inputs;check_payloads;log 'all input and payload hashes match; no mounts performed';;
 activate)
  trial_seconds=${2:-180}
  case "$trial_seconds" in ''|*[!0-9]*) fail 'invalid trial duration';;esac
  [ "$trial_seconds" -ge 60 ] && [ "$trial_seconds" -le 900 ] || fail 'trial duration must be 60..900 seconds'
  [ ! -f "$STATE/active" ] && [ ! -f "$STATE/armed" ] || fail 'already active or armed'
  check_inputs;check_payloads
  if [ -f /data/adb/s25_bluetooth/config.conf ];then "$BB" sh "$MODDIR/config.sh" validate;fi
  [ -f "$MODDIR/proofs/offline-ready" ] || fail 'offline integration proof missing'
  zpid=$(host_zygote);[ -n "$zpid" ] || fail 'zygote64 unavailable'
  # Verify the app parent's view before the first mutation.
  while IFS=' ' read -r digest path; do
   [ -n "$digest" ] || continue
   actual=$(nsrun "$zpid" "$BB" sha256sum "$path");actual=${actual%% *}
   [ "$actual" = "$digest" ] || fail "zygote namespace mismatch: $path"
  done < "$MODDIR/inputs.sha256"
  : > "$STATE/mounts.tsv"
  rm -f "$STATE/hci-and-audio-verified"
  if dumpsys bluetooth_manager | "$BB" awk '/^  State:/ {print $2}' | grep -qx ON;then echo 1 > "$STATE/radio-on";else echo 0 > "$STATE/radio-on";fi
  save_properties
  token=$(cat /proc/sys/kernel/random/uuid);echo "$token" > "$STATE/armed"
  "$BB" setsid "$BB" sh "$MODDIR/watchdog.sh" "$MODDIR" "$trial_seconds" "$token" 9>&- > "$STATE/watchdog.log" 2>&1 < /dev/null &
  echo $! > "$STATE/watchdog.pid"
  trap 'rollback' HUP INT TERM
  trap '[ "$?" = 0 ] || rollback' EXIT
  qhs_enabled=false
  if [ -f /data/adb/s25_bluetooth/config.conf ];then qhs_enabled=$("$BB" sh "$MODDIR/config.sh" value classic_qhs_host_aware);fi
  if [ "$qhs_enabled" = true ];then
   [ "$(cat "$STATE/radio-on")" = 1 ] || fail 'QHS activation requires Bluetooth ON for capability readback'
   "$BB" sh "$MODDIR/qhs-nvm.sh" apply
  fi
  quiesce_radio || fail 'Bluetooth did not enter OFF state before HAL restart'
  # Labels match the original inodes; never create a new target path.
  chown 0:0 "$MODDIR/payload/"*.so "$MODDIR/payload/"*.apk;chmod 0444 "$MODDIR/payload/"*.so "$MODDIR/payload/"*.apk
  chcon u:object_r:system_lib_file:s0 "$MODDIR/payload/patched-libbluetooth_jni.so"
  chcon u:object_r:vendor_file:s0 "$MODDIR/payload/patched-libsehbluetooth_audio_session_aidl.so"
  chcon u:object_r:system_file:s0 "$MODDIR/payload/patched-Bluetooth.apk"
  for target_pid in 1 "$zpid";do
   if [ "$target_pid" = 1 ];then check_target=$HAL;else check_target=$NATIVE;fi
   if grep -F " $check_target " "/proc/$target_pid/mountinfo" >/dev/null;then fail "existing file mount at target in namespace $target_pid";fi
   nsid=$("$BB" readlink "/proc/$target_pid/ns/mnt")
   if [ "$target_pid" = 1 ];then
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/patched-libsehbluetooth_audio_session_aidl.so" "$HAL"
    echo "$target_pid $nsid $HAL" >> "$STATE/mounts.tsv"
   else
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/patched-libbluetooth_jni.so" "$NATIVE"
    echo "$target_pid $nsid $NATIVE" >> "$STATE/mounts.tsv"
    if grep -F " $APK " "/proc/$target_pid/mountinfo" >/dev/null;then fail 'existing APK mount';fi
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/patched-Bluetooth.apk" "$APK"
    echo "$target_pid $nsid $APK" >> "$STATE/mounts.tsv"
   fi
  done
  /data/adb/ksu/bin/resetprop -n persist.bluetooth.samsung.a2dp_offload.cap "$(cat "$STATE/samsung-cap")-aptx_hd"
  /data/adb/ksu/bin/resetprop -n persist.bluetooth.btsnooplogmode full
  source_priority=4001
  if [ -f /data/adb/s25_bluetooth/config.conf ];then source_priority=$("$BB" sh "$MODDIR/config.sh" value adaptive_priority);fi
  /data/adb/ksu/bin/resetprop -n bluetooth.a2dp.source.aptx_adaptive_priority.config "$source_priority"
  voice_enabled=false
  if [ -f /data/adb/s25_bluetooth/config.conf ];then voice_enabled=$("$BB" sh "$MODDIR/config.sh" value aptx_voice_enabled);fi
  /data/adb/ksu/bin/resetprop -n bluetooth.hfp.codec_aptx_voice.enabled "$voice_enabled"
  hd_priority=4001
  if [ -f /data/adb/s25_bluetooth/config.conf ];then hd_priority=$("$BB" sh "$MODDIR/config.sh" value aptx_hd_priority);fi
  /data/adb/ksu/bin/resetprop -n bluetooth.a2dp.source.aptx_hd_priority.config "$hd_priority"
  case "$(cat "$MODDIR/payload/runtime.prop")" in
   persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support=true) r21_value=true;;
   persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support=false) r21_value=false;;
   *) fail 'unexpected runtime property payload';;
  esac
  /data/adb/ksu/bin/resetprop -n persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support "$r21_value"
  if [ -f "$MODDIR/.enable-dual-mode" ];then /data/adb/ksu/bin/resetprop -n persist.bluetooth.enable_dual_mode_audio true;fi
  old_bt=$(host_bt || true)
  old_hal=$(getprop init.svc_debug_pid.vendor.audio-hal-aidl)
  setprop ctl.restart vendor.audio-hal-aidl
  hal_ready=0;hal_attempt=0
  while [ "$hal_attempt" -lt 30 ];do
   candidate_hal=$(getprop init.svc_debug_pid.vendor.audio-hal-aidl)
   if [ -n "$candidate_hal" ] && [ "$candidate_hal" != "$old_hal" ] && grep -q libsehbluetooth_audio_session_aidl.so "/proc/$candidate_hal/maps" 2>/dev/null;then
    if "$BB" timeout 2 service call vendor.samsung.hardware.bluetooth.audio.ISehBluetoothAudioProviderFactory/default 1599098439 2>/dev/null | grep -q 'Result: Parcel';then hal_ready=1;break;fi
   fi
   sleep 1;hal_attempt=$((hal_attempt+1))
  done
  [ "$hal_ready" = 1 ] || fail 'audio HAL factory readiness timeout'
  old_bt=$(host_bt || true)
  [ -z "$old_bt" ] || kill -TERM $old_bt 2>/dev/null || true
  if [ "$qhs_enabled" = true ];then "$BB" sh "$MODDIR/qhs-nvm.sh" scan-restore;fi
  resume_radio
  loaded=0
  attempt=0
  while [ "$attempt" -lt 35 ];do
   new_bt=$(host_bt || true)
   if [ -n "$new_bt" ] && [ "$new_bt" != "$old_bt" ];then
    if grep -q libbluetooth_jni.so "/proc/$new_bt/maps";then
     actual=$(nsrun "$new_bt" "$BB" sha256sum "$NATIVE");actual=${actual%% *}
     expected=$(sha "$MODDIR/payload/patched-libbluetooth_jni.so")
     [ "$actual" = "$expected" ] || fail 'Bluetooth child did not inherit the exact payload'
     nsid=$("$BB" readlink "/proc/$new_bt/ns/mnt");echo "$new_bt $nsid $NATIVE" >> "$STATE/mounts.tsv"
     actual=$(nsrun "$new_bt" "$BB" sha256sum "$APK");actual=${actual%% *}
     [ "$actual" = "$(sha "$MODDIR/payload/patched-Bluetooth.apk")" ] || fail 'Bluetooth child APK payload mismatch'
     echo "$new_bt $nsid $APK" >> "$STATE/mounts.tsv"
     record_process bt "$new_bt";loaded=1;break
    fi
   fi
   sleep 1;attempt=$((attempt+1))
  done
  [ "$loaded" = 1 ] || fail 'Bluetooth payload load timeout'
  sleep 8
  [ "$(host_bt || true)" = "$new_bt" ] || fail 'Bluetooth did not remain stable during initialization'
  new_hal=$(getprop init.svc_debug_pid.vendor.audio-hal-aidl)
  [ -n "$new_hal" ] && [ "$new_hal" != "$old_hal" ] || fail 'audio HAL did not restart'
  grep -q libsehbluetooth_audio_session_aidl.so "/proc/$new_hal/maps" || fail 'audio HAL library not mapped'
  actual=$(nsrun "$new_hal" "$BB" sha256sum "$HAL");actual=${actual%% *}
  [ "$actual" = "$(sha "$MODDIR/payload/patched-libsehbluetooth_audio_session_aidl.so")" ] || fail 'audio HAL payload mismatch'
  record_process hal "$new_hal"
  # Future applications and Bluetooth restarts inherit the stock parent view.
  nsrun "$zpid" "$BB" umount -l "$NATIVE"
  nsrun "$zpid" "$BB" umount -l "$APK"
  if [ "$qhs_enabled" = true ];then qhs_features true;fi
  touch "$STATE/active"
  log "loaded experiment; watchdog will restore stock after $trial_seconds seconds unless committed"
  ;;
 keep-verified)
  [ -f "$STATE/active" ] || fail 'not active'
  [ -f "$MODDIR/proofs/verified-payloads.sha256" ] || fail 'heard/verified payload proof missing'
  "$BB" cmp -s "$MODDIR/payloads.sha256" "$MODDIR/proofs/verified-payloads.sha256" || fail 'unverified payload generation'
  verify_active_payloads
  start_health;rm -f "$STATE/armed";start_policy;log 'kept exact tested payload; see proofs/audio-verification.json for audio evidence scope';;
 commit)
  [ -f "$STATE/active" ] || fail 'not active'
  [ -f "$STATE/hci-and-audio-verified" ] || fail 'HCI and audio success have not been verified'
  verify_active_payloads
  start_health;rm -f "$STATE/armed";start_policy;log 'kept current experimental process; automatic boot activation stays disabled';;
 rollback) rollback;;
 rollback-trial|rollback-health)
  [ "$#" = 2 ] || fail 'watcher recovery requires a generation token'
  rollback "${1#rollback-}" "$2";;
 *) fail 'usage: control.sh status|preflight|activate|commit|rollback';;
esac
