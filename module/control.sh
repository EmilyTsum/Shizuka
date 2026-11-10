#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
[ -x "$BB" ] || { echo 'KernelSU busybox unavailable' >&2; exit 1; }
log() { echo "s25_aptx_adaptive: $*"; }
fail() { log "$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || fail 'root required'
. "$MODDIR/lib/runtime.sh"
ensure_busybox_shell "$@"
STATE="$MODDIR/state"
. "$MODDIR/lib/device.sh"
. "$MODDIR/lib/properties.sh"
. "$MODDIR/lib/config.sh"
. "$MODDIR/lib/policy.sh"
# Serialize activation, commit and recovery, including watchdog requests.
case "${1:-}" in
 activate|commit|keep-verified|rollback|rollback-trial|rollback-health) lock_lifecycle;;
esac
case "${1:-}" in
 activate|commit|keep-verified)
  application_fence check > "$STATE/latency-application-fence.json" || fail 'An unresolved delay operation requires owned recovery before activation or commit'
  ;;
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
  return 1
 fi
 return 0
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
start_policy() (
 # The caller already holds the lifecycle lock; do not reacquire it in a child.
 lock_policy
 start_policy_worker
)
keep_session() {
 verify_active_payloads || return 1
 start_health || return 1
 start_policy || return 1
 rm -f "$STATE/armed"
}
allowed() { profile_target_allowed "$1"; }

check_inputs() {
 [ -s "$MODDIR/inputs.sha256" ] || fail 'profile missing'
 count=0
 while IFS=' ' read -r digest path; do
  [ -n "$digest" ] || continue
  allowed "$path" || fail "unexpected target: $path"
  [ "$(sha "$path")" = "$digest" ] || fail "input hash mismatch: $path"
  count=$((count+1))
 done < "$MODDIR/inputs.sha256"
 [ "$count" = "$STOCK_TARGET_COUNT" ] || fail 'incomplete profile'
}
check_payloads() {
 [ -s "$MODDIR/payloads.sha256" ] || fail 'payload manifest missing'
 count=0
 while IFS=' ' read -r digest name; do
  profile_payload_allowed "$name" || fail 'unexpected payload name'
  [ "$(sha "$MODDIR/payload/$name")" = "$digest" ] || fail "payload hash mismatch: $name"
  count=$((count+1))
 done < "$MODDIR/payloads.sha256"
 [ "$count" = 4 ] || fail 'incomplete payload manifest'
}
qhs_features() {
 "$BB" sh "$MODDIR/config.sh" controller-features > "$STATE/qhs-controller-features.json"
 "$BB" grep -Eq '"local_enabled"[[:space:]]*:[[:space:]]*'"$1"'([,}[:space:]])' "$STATE/qhs-controller-features.json" || fail 'controller QHS local capability readback mismatch'
 case "$1" in true) qhs_hex=$QHS_ENABLED_FEATURES;;false) qhs_hex=$QHS_DISABLED_FEATURES;;*) return 1;;esac
 "$BB" grep -Eq '"features_hex"[[:space:]]*:[[:space:]]*"'"$qhs_hex"'"' "$STATE/qhs-controller-features.json" || fail 'controller feature bytes differ from tested profile'

}
reconcile_application() {
 if application_fence check > "$STATE/latency-application-fence.json" 2>&1;then return 0;fi
 # Read-only physical verification plus metadata finalization. FD9 is inherited
 # from this normal controller; the resident guardian's authority is not reused.
 application_fence reconcile > "$STATE/latency-application-reconcile.json" 2>&1 || true
 application_fence check > "$STATE/latency-application-fence.json" 2>&1
}
verify_stock_recovery() {
 # This boundary is independent of the application reader/receipt. Retire
 # physical markers only after every owned resource is actually normal again.
 check_inputs || return 1
 ! qhs_pending || return 1
 for stock_marker in latency-runtime.active latency-remote-uncertain latency-provider.pid latency-provider.start dsp-trial.active;do
  [ ! -e "$STATE/$stock_marker" ] && [ ! -L "$STATE/$stock_marker" ] || return 1
 done
 command -v profile_latency_recovery_preflight >/dev/null 2>&1 && \
  command -v profile_latency_normal_ready >/dev/null 2>&1 || return 1
 stock_boot=$(cat /proc/sys/kernel/random/boot_id) || return 1
 printf '%s\n' "$stock_boot" | "$BB" grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' || return 1
 stock_hal_start=$(process_start "$next_hal") || return 1
 profile_latency_recovery_preflight && profile_latency_normal_ready "$next_hal" "$previous_hal" || return 1
 stock_zygote=$(host_zygote) || return 1
 case "$stock_zygote" in ''|*[!0-9]*|0) return 1;;esac
 stock_bt=$(host_bt || true);stock_bt_start=''
 if [ -n "$stock_bt" ];then
  case "$stock_bt" in *[!0-9]*|0) return 1;;esac
  stock_bt_start=$(process_start "$stock_bt") || return 1
 fi
 stock_count=0;stock_seen=' '
 while IFS=' ' read -r stock_expected stock_target stock_extra;do
  [ -n "$stock_expected" ] || continue
  [ -z "${stock_extra:-}" ] && [ "${#stock_expected}" = 64 ] || return 1
  case "$stock_expected" in *[!0-9a-f]*) return 1;;esac
  allowed "$stock_target" || return 1
  case "$stock_seen" in *" $stock_target "*) return 1;;esac
  stock_seen="$stock_seen$stock_target ";stock_count=$((stock_count+1))
  stock_actual=$(nsrun 1 "$BB" sha256sum "$stock_target") || return 1
  [ "${stock_actual%% *}" = "$stock_expected" ] || return 1
  case "$stock_target" in
   "$NATIVE"|"$APK")
    stock_actual=$(nsrun "$stock_zygote" "$BB" sha256sum "$stock_target") || return 1
    [ "${stock_actual%% *}" = "$stock_expected" ] || return 1
    if [ -n "$stock_bt" ];then
     stock_actual=$(nsrun "$stock_bt" "$BB" sha256sum "$stock_target") || return 1
     [ "${stock_actual%% *}" = "$stock_expected" ] || return 1
    fi;;
   "$HAL")
    stock_actual=$(nsrun "$next_hal" "$BB" sha256sum "$stock_target") || return 1
    [ "${stock_actual%% *}" = "$stock_expected" ] || return 1;;
  esac
 done < "$MODDIR/inputs.sha256"
 [ "$stock_count" = "$STOCK_TARGET_COUNT" ] && [ "$stock_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] && \
  [ "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" = "$next_hal" ] && process_matches "$next_hal" "$stock_hal_start" || return 1
 [ "$(host_bt || true)" = "$stock_bt" ] || return 1
 [ -z "$stock_bt" ] || process_matches "$stock_bt" "$stock_bt_start"
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
 application_resolved=true
 if ! reconcile_application;then application_resolved=false;fi
 if ! physical_recovery_pending;then
  check_inputs || return 1
  if [ "$application_resolved" != true ];then
   log 'application ownership remains fenced; no physical recovery markers are present; no audio state was changed'
   return 1
  fi
  log 'already inactive; stock hashes match';return
 fi
 (lock_policy;stop_policy) || return 1
 rm -f "$STATE/health-token"
 if command -v profile_stop_runtime >/dev/null 2>&1;then profile_stop_runtime || return 1;fi
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
 previous_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
 # Device experiments may need their loader reset after their owned mounts are removed.
 if command -v profile_recover_experiments >/dev/null 2>&1;then
  profile_recover_experiments || return 1
 fi
 setprop ctl.restart "$AUDIO_HAL_SERVICE"
 hal_wait=0
 while [ "$hal_wait" -lt 20 ];do
  next_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  if profile_hal_ready "$next_hal" "$previous_hal";then break;fi
  sleep 1;hal_wait=$((hal_wait+1))
 done
 profile_hal_ready "$next_hal" "$previous_hal" || return 1
 if command -v profile_finish_runtime_recovery >/dev/null 2>&1;then
  profile_finish_runtime_recovery "$next_hal" || return 1
 fi
 [ -z "$current_bt" ] || kill -TERM $current_bt 2>/dev/null || true
 sleep 3
 if [ "$qhs_restoring" = 1 ];then "$BB" sh "$MODDIR/qhs-nvm.sh" scan-restore;fi
 resume_radio
 if [ "$qhs_restoring" = 1 ];then
  qhs_features false
  "$BB" sh "$MODDIR/qhs-nvm.sh" finish-remove
 fi
 verify_stock_recovery || return 1
 rm -f "$STATE/active" "$STATE/experiment-unverified"
 : > "$STATE/mounts.tsv"
 # Physical recovery is independently complete. Retire its markers even if a
 # separate application receipt is unavailable, so an orphan cannot cause every
 # later watchdog/manual attempt to restart already-normal audio again.
 rm -f "$STATE/armed"
 if [ -f "$STATE/watchdog.pid" ];then
  old_watch=$(cat "$STATE/watchdog.pid")
  if [ -f "/proc/$old_watch/cmdline" ] && "$BB" tr '\000' ' ' < "/proc/$old_watch/cmdline" | grep -F "$MODDIR/watchdog.sh" >/dev/null;then kill -TERM "$old_watch" 2>/dev/null || true;fi
 fi
 if ! reconcile_application;then
  log 'stock audio recovery verified; application metadata remains fenced; further recovery attempts will only verify metadata'
  return 1
 fi
 log 'verified stock hashes after unmount; restored properties and restarted audio/Bluetooth'
)
latency_application_entry() {
 entry_command=$1;shift
 operation_json=null
 if [ "$entry_command" = latency-apply ];then
  [ "$#" = 3 ] || fail 'latency-apply requires UUID HQ_MS LL_MS'
 else
  [ "$#" -le 1 ] || fail 'latency-apply-status accepts one optional UUID'
 fi
 if [ "$#" -gt 0 ];then
  [ "${#1}" = 36 ] || fail 'Invalid delay operation identity'
  printf '%s\n' "$1" | "$BB" grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' || fail 'Invalid delay operation identity'
  operation_json="\"$1\""
 fi
 if [ "$entry_command" = latency-apply ];then
  for delay_value in "$2" "$3";do
   case "$delay_value" in 0|[1-9]|[1-9][0-9]|1[0-4][0-9]|150) ;;*) fail 'Delay must be a canonical integer from 0 to 150 ms';;esac
  done
 fi
 if [ -f "$MODDIR/latency-apply.sh" ] && [ ! -L "$MODDIR/latency-apply.sh" ] && \
  [ -f "$MODDIR/latency-application.dex" ] && [ -f "$MODDIR/latency-application.sha256" ];then
  if [ "$entry_command" = latency-apply ];then
   exec "$BB" sh "$MODDIR/latency-apply.sh" begin "$@"
  else
   exec "$BB" sh "$MODDIR/latency-apply.sh" status "$@"
  fi
 fi
 # Absence cannot reconcile an earlier operation or authorize another mutation.
 entry_boot=$(cat /proc/sys/kernel/random/boot_id)
 [ "${#entry_boot}" = 36 ] || fail 'Invalid current boot identity'
 printf '%s\n' "$entry_boot" | "$BB" grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' || fail 'Invalid current boot identity'
 printf '{"schema":1,"scope":"latency_application","operation_id":%s,"operation_known":false,"current_operation_id":null,"current_boot_id":"%s","available":false,"current_context_supported":false,"disable_available":false,"accepted":false,"phase":"unavailable","terminal":true,"retry_safe":false,"applied":false,"disabled":false,"fresh":false,"lease_valid":false,"disable_receipt_valid":false,"current_config_matches":false,"pending_hq":true,"pending_ll":true,"reason":"The application helper is unavailable. Existing ownership has not been reconciled."}\n' "$operation_json" "$entry_boot"
}
latency_owned_entry() {
 owned_command=$1;shift
 [ "$#" = 10 ] || fail 'Invalid owned delay action arguments'
 [ -f "$MODDIR/latency-apply-owned.sh" ] && [ ! -L "$MODDIR/latency-apply-owned.sh" ] || return 10
 # The fixed handler verifies the inherited FD9 inode, guardian identity,
 # private operation/capture bindings and deadline. Never reacquire FD9 here.
 if [ "$owned_command" = latency-owned ];then
  exec "$BB" sh "$MODDIR/latency-apply-owned.sh" execute "$@"
 fi
 if "$BB" sh "$MODDIR/latency-apply-owned.sh" validate-health "$@";then
  start_health
 else
  return $?
 fi
}
case "${1:-status}" in
 latency-apply|latency-apply-status) latency_application_entry "$@";;
 latency-owned|latency-owned-health) latency_owned_entry "$@";;
 status)
  if [ -f "$STATE/active" ];then
   verify_active_payloads
   log "active; exact APK/native/HAL hashes verified; Bluetooth=$live_bt HAL=$live_hal"
   if [ -f "$STATE/armed" ];then log 'trial: watchdog armed';fi
  else
   check_inputs
   if recovery_pending;then fail 'recovery pending; use Action or rollback to restore stock';fi
   log 'stock hashes match; inactive'
  fi
  ;;
 preflight) check_inputs;check_payloads;log 'all input and payload hashes match; no mounts performed';;
 activate)
  trial_seconds=${2:-180}
  case "$trial_seconds" in ''|*[!0-9]*) fail 'invalid trial duration';;esac
  [ "$trial_seconds" -ge 60 ] && [ "$trial_seconds" -le 900 ] || fail 'trial duration must be 60..900 seconds'
  if recovery_pending;then fail 'active or unfinished recovery; run rollback before activating';fi
  check_inputs;check_payloads
  if config_exists;then "$BB" sh "$MODDIR/config.sh" validate;fi
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
  qhs_enabled=$(extension_value classic_qhs_host_aware)
  if [ "$qhs_enabled" = true ];then
   [ "${QHS_SUPPORTED:-false}" = true ] || fail 'QHS is unsupported by this device backend'
   [ "$(cat "$STATE/radio-on")" = 1 ] || fail 'QHS activation requires Bluetooth ON for capability readback'
   "$BB" sh "$MODDIR/qhs-nvm.sh" apply
  fi
  quiesce_radio || fail 'Bluetooth did not enter OFF state before HAL restart'
  # Labels match the original inodes; never create a new target path.
  chown 0:0 "$MODDIR/payload/"*.so "$MODDIR/payload/"*.apk;chmod 0444 "$MODDIR/payload/"*.so "$MODDIR/payload/"*.apk
  chcon "$NATIVE_LABEL" "$MODDIR/payload/$NATIVE_PAYLOAD"
  chcon "$HAL_LABEL" "$MODDIR/payload/$HAL_PAYLOAD"
  chcon "$APK_LABEL" "$MODDIR/payload/$APK_PAYLOAD"
  for target_pid in 1 "$zpid";do
   if [ "$target_pid" = 1 ];then check_target=$HAL;else check_target=$NATIVE;fi
   if grep -F " $check_target " "/proc/$target_pid/mountinfo" >/dev/null;then fail "existing file mount at target in namespace $target_pid";fi
   nsid=$("$BB" readlink "/proc/$target_pid/ns/mnt")
   if [ "$target_pid" = 1 ];then
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/$HAL_PAYLOAD" "$HAL"
    echo "$target_pid $nsid $HAL" >> "$STATE/mounts.tsv"
   else
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/$NATIVE_PAYLOAD" "$NATIVE"
    echo "$target_pid $nsid $NATIVE" >> "$STATE/mounts.tsv"
    if grep -F " $APK " "/proc/$target_pid/mountinfo" >/dev/null;then fail 'existing APK mount';fi
    nsrun "$target_pid" "$BB" mount -o bind "$MODDIR/payload/$APK_PAYLOAD" "$APK"
    echo "$target_pid $nsid $APK" >> "$STATE/mounts.tsv"
   fi
  done
  if command -v profile_activate_experiments >/dev/null 2>&1;then
   profile_activate_experiments || fail 'device experiment activation failed'
  fi
  profile_apply_properties
  old_bt=$(host_bt || true)
  old_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  setprop ctl.restart "$AUDIO_HAL_SERVICE"
  hal_ready=0;hal_attempt=0
  while [ "$hal_attempt" -lt 30 ];do
   candidate_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
   if profile_hal_ready "$candidate_hal" "$old_hal";then hal_ready=1;break;fi
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
    if grep -Fq "${NATIVE##*/}" "/proc/$new_bt/maps";then
     actual=$(nsrun "$new_bt" "$BB" sha256sum "$NATIVE");actual=${actual%% *}
     expected=$(sha "$MODDIR/payload/$NATIVE_PAYLOAD")
     [ "$actual" = "$expected" ] || fail 'Bluetooth child did not inherit the exact payload'
     nsid=$("$BB" readlink "/proc/$new_bt/ns/mnt");echo "$new_bt $nsid $NATIVE" >> "$STATE/mounts.tsv"
     actual=$(nsrun "$new_bt" "$BB" sha256sum "$APK");actual=${actual%% *}
     [ "$actual" = "$(sha "$MODDIR/payload/$APK_PAYLOAD")" ] || fail 'Bluetooth child APK payload mismatch'
     echo "$new_bt $nsid $APK" >> "$STATE/mounts.tsv"
     record_process bt "$new_bt";loaded=1;break
    fi
   fi
   sleep 1;attempt=$((attempt+1))
  done
  [ "$loaded" = 1 ] || fail 'Bluetooth payload load timeout'
  sleep 8
  [ "$(host_bt || true)" = "$new_bt" ] || fail 'Bluetooth did not remain stable during initialization'
  new_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  [ -n "$new_hal" ] && [ "$new_hal" != "$old_hal" ] || fail 'audio HAL did not restart'
  grep -Fq "${HAL##*/}" "/proc/$new_hal/maps" || fail 'audio HAL library not mapped'
  actual=$(nsrun "$new_hal" "$BB" sha256sum "$HAL");actual=${actual%% *}
  [ "$actual" = "$(sha "$MODDIR/payload/$HAL_PAYLOAD")" ] || fail 'audio HAL payload mismatch'
  record_process hal "$new_hal"
  # Future applications and Bluetooth restarts inherit the stock parent view.
  nsrun "$zpid" "$BB" umount -l "$NATIVE"
  nsrun "$zpid" "$BB" umount -l "$APK"
  if [ "$qhs_enabled" = true ];then qhs_features true;fi
  touch "$STATE/active"
  if command -v profile_start_runtime >/dev/null 2>&1;then profile_start_runtime || fail 'device runtime publisher failed';fi
  if [ -e "$STATE/latency-runtime.active" ];then start_health || fail 'device runtime health watcher failed';fi
  log "loaded experiment; watchdog will restore stock after $trial_seconds seconds unless committed"
  ;;
 keep-verified)
  [ ! -e "$STATE/experiment-unverified" ] || fail 'device experiment requires fresh audio verification'
  [ -f "$STATE/active" ] || fail 'not active'
  [ -f "$MODDIR/proofs/verified-payloads.sha256" ] || fail 'heard/verified payload proof missing'
  "$BB" cmp -s "$MODDIR/payloads.sha256" "$MODDIR/proofs/verified-payloads.sha256" || fail 'unverified payload generation'
  keep_session
  log 'kept exact tested payload; see proofs/audio-verification.json for audio evidence scope';;
 commit)
  [ -f "$STATE/active" ] || fail 'not active'
  [ -f "$STATE/hci-and-audio-verified" ] || fail 'HCI and audio success have not been verified'
  keep_session
  rm -f "$STATE/experiment-unverified"
  log 'kept current experimental process; automatic boot activation stays disabled';;
 rollback) rollback;;
 rollback-trial|rollback-health)
  [ "$#" = 2 ] || fail 'watcher recovery requires a generation token'
  rollback "${1#rollback-}" "$2";;
 *) fail 'usage: control.sh status|preflight|activate|commit|rollback';;
esac
