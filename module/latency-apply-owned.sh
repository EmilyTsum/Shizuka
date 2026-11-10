#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixed HAL-only lifecycle owner. This source gate is intentionally false.
set -eu
MODDIR=${0%/*};STATE="$MODDIR/state";BB=/data/adb/ksu/bin/busybox
APP_PROCESS=/system/bin/app_process
QUALIFIED=0
owned_fail(){ echo "Shizuka owned delay: $*" >&2;exit 20; }
[ "$#" = 11 ] || owned_fail 'fixed mode and ten typed arguments required'
owned_mode=$1;shift
case "$owned_mode" in execute|validate-request|validate-health) ;;*) owned_fail 'unknown fixed mode';;esac
[ -x "$BB" ] && [ "$(id -u)" = 0 ] || owned_fail 'current root is required'
[ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || owned_fail 'module path must be canonical'
owned_trusted(){
 [ -f "$1" ] && [ ! -L "$1" ] || owned_fail 'unsafe fixed file'
 owned_attrs=$("$BB" stat -c '%u:%a:%h' "$1") || owned_fail 'cannot inspect fixed file'
 case "$owned_attrs" in 0:*:1) ;;*) owned_fail 'unsafe owner/link count';;esac
 owned_permissions=${owned_attrs#0:};owned_permissions=${owned_permissions%:1}
 case "$owned_permissions" in ''|*[!0-7]*) owned_fail 'invalid fixed permissions';;esac
 [ "$((0$owned_permissions & 022))" = 0 ] || owned_fail 'writable fixed file'
}
owned_trusted "$MODDIR/lib/runtime.sh"
. "$MODDIR/lib/runtime.sh";fail(){ owned_fail "$*"; }
ensure_busybox_shell "$owned_mode" "$@"
# No environment/property/file can enable this gate. The read-only guard modes
# also stay disabled so request preparation and health rotation cannot bypass it.
[ "$QUALIFIED" = 1 ] || exit 10
owned_action=$1;owned_id=$2;owned_hq=$3;owned_ll=$4;owned_deadline=$5
owned_token=$6;owned_config=$7;owned_flag=$8;owned_guardian=$9;shift 9;owned_guardian_start=$1
owned_call(){ "$@" "$owned_action" "$owned_id" "$owned_hq" "$owned_ll" "$owned_deadline" "$owned_token" "$owned_config" "$owned_flag" "$owned_guardian" "$owned_guardian_start"; }
owned_trusted "$MODDIR/latency-application.dex";owned_trusted "$MODDIR/latency-application.sha256"
[ "$("$BB" stat -c '%s' "$MODDIR/latency-application.sha256")" = 65 ] || owned_fail 'invalid helper digest length'
[ "$(sha "$MODDIR/latency-application.dex")" = "$(cat "$MODDIR/latency-application.sha256")" ] || owned_fail 'helper digest mismatch'
owned_java(){ CLASSPATH="$MODDIR/latency-application.dex" "$APP_PROCESS" /system/bin "$@"; }
case "$owned_mode" in execute) owned_guard_mode=action;;validate-request) owned_guard_mode=request;;validate-health) owned_guard_mode=health;;esac
owned_call owned_java LatencyOwnedGuardMain --module-dir "$MODDIR" "$owned_guard_mode" || exit 20
[ "$owned_mode" = execute ] || exit 0
owned_trusted "$MODDIR/lib/device.sh";owned_trusted "$MODDIR/device-profile";owned_trusted "$MODDIR/profiles/samsung-sm-s931z-dzif.sh"
[ "$(cat "$MODDIR/device-profile")" = samsung-sm-s931z-dzif ] || owned_fail 'unreviewed fixed backend'
. "$MODDIR/lib/device.sh"
GSL=/vendor/lib64/libar-gsl.so
GSL_STOCK=4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc
owned_trusted "$STATE/bt.pid";owned_trusted "$STATE/bt.start"
owned_bt=$(cat "$STATE/bt.pid");owned_bt_start=$(cat "$STATE/bt.start")
owned_bt_same(){ [ "$(host_bt)" = "$owned_bt" ] && process_matches "$owned_bt" "$owned_bt_start"; }
owned_check_time(){
 owned_now=$(owned_java LatencyMonotonicMain) || return 1
 case "$owned_now" in ''|*[!0-9]*) return 1;;esac
 [ "$owned_now" -lt "$owned_deadline" ]
}
owned_step(){ owned_check_time && owned_bt_same || owned_fail 'deadline or captured Bluetooth generation changed'; }
owned_receipt(){ owned_call owned_java LatencyOwnerReceiptMain --module-dir "$MODDIR"; }
owned_runtime(){ "$BB" sh "$MODDIR/latency-runtime.sh" "$@"; }
owned_health(){ owned_call "$BB" sh "$MODDIR/control.sh" latency-owned-health; }
owned_payload_hal(){
 owned_live=$(nsrun "$1" "$BB" sha256sum "$HAL") || return 1
 [ "${owned_live%% *}" = "$(sha "$MODDIR/payload/$HAL_PAYLOAD")" ]
}
owned_read_owner(){
 owned_trusted "$STATE/latency-application/owned.$owned_id"
 IFS=' ' read -r own_version own_id own_boot own_cookie own_token own_config own_hq own_ll own_pid own_ticks own_old_hal own_old_start own_new_hal own_new_start own_graph own_phase own_capture own_package own_activation own_extra < "$STATE/latency-application/owned.$owned_id" || owned_fail 'owner receipt unavailable'
 [ "$own_version" = 1 ] && [ "$own_id" = "$owned_id" ] && [ -z "${own_extra:-}" ] || owned_fail 'owner receipt changed'
}
owned_clear_request(){
 if [ -e "$STATE/latency-runtime.request" ] || [ -L "$STATE/latency-runtime.request" ];then
  owned_trusted "$STATE/latency-runtime.request"
  [ "$("$BB" stat -c '%u:%a:%h' "$STATE/latency-runtime.request")" = 0:600:1 ] || owned_fail 'unsafe runtime request'
  owned_expected="$own_boot $own_cookie $(sha "$MODDIR/payload/patched-libar-gsl.so") relay"
  [ "$(cat "$STATE/latency-runtime.request")" = "$owned_expected" ] && [ "$("$BB" stat -c '%s' "$STATE/latency-runtime.request")" = "$((${#owned_expected}+1))" ] || owned_fail 'another request owns this runtime'
  rm "$STATE/latency-runtime.request"
 fi
}
owned_unmount_gsl(){
 owned_trusted "$STATE/mounts.tsv"
 [ "$("$BB" stat -c '%s' "$STATE/mounts.tsv")" -le 65536 ] || owned_fail 'mount ledger exceeds bound'
 owned_ledger=$("$BB" mktemp "$STATE/.application-mounts.XXXXXX") || owned_fail 'cannot stage mount ledger'
 chmod 0600 "$owned_ledger";owned_gsl_count=0
 while IFS=' ' read -r ledger_pid ledger_namespace ledger_target ledger_extra;do
  [ -n "$ledger_pid" ] || continue
  case "$ledger_pid" in *[!0-9]*|0) owned_fail 'invalid tracked namespace PID';;esac
  [ -z "${ledger_extra:-}" ] || owned_fail 'invalid mount ledger fields'
  printf '%s\n' "$ledger_namespace" | "$BB" grep -Eq '^mnt:\[[0-9]+\]$' || owned_fail 'invalid tracked namespace'
  if [ "$ledger_target" = "$GSL" ];then
   owned_gsl_count=$((owned_gsl_count+1))
   [ "$owned_gsl_count" = 1 ] && [ "$ledger_pid" = 1 ] && [ "$ledger_namespace" = "$("$BB" readlink /proc/1/ns/mnt)" ] || owned_fail 'unreviewed GSL mount owner'
  else
   profile_target_allowed "$ledger_target" || owned_fail 'unreviewed retained core mount'
   printf '%s %s %s\n' "$ledger_pid" "$ledger_namespace" "$ledger_target" >> "$owned_ledger"
  fi
 done < "$STATE/mounts.tsv"
 owned_hash=$(nsrun 1 "$BB" sha256sum "$GSL");owned_hash=${owned_hash%% *}
 if [ "$owned_gsl_count" = 1 ] && [ "$owned_hash" = "$(sha "$MODDIR/payload/patched-libar-gsl.so")" ];then
  grep -F " $GSL " /proc/1/mountinfo >/dev/null || owned_fail 'tracked GSL overlay is not mounted'
  nsrun 1 "$BB" umount "$GSL" || owned_fail 'owned GSL unmount failed'
 fi
 owned_hash=$(nsrun 1 "$BB" sha256sum "$GSL");owned_hash=${owned_hash%% *}
 [ "$owned_hash" = "$GSL_STOCK" ] && ! grep -F " $GSL " /proc/1/mountinfo >/dev/null || owned_fail 'stock GSL is not verified after owned unmount'
 mv "$owned_ledger" "$STATE/mounts.tsv";owned_ledger=''
}
owned_start_hal(){
 owned_hal_mode=$1;owned_step
 owned_identity=$(profile_latency_hal_identity) || owned_fail 'HAL identity unavailable'
 IFS=' ' read -r stopped_hal stopped_ticks stopped_extra <<EOF
$owned_identity
EOF
 [ -z "${stopped_extra:-}" ] || owned_fail 'HAL identity has extra fields'
 profile_latency_stop_hal "$stopped_hal" "$stopped_ticks" || owned_fail 'bound HAL did not stop'
 owned_step
 setprop ctl.start "$AUDIO_HAL_SERVICE" || owned_fail 'normal HAL start failed'
 owned_attempt=0;owned_ready=0
 while [ "$owned_attempt" -lt 30 ];do
  owned_step
  next_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  if profile_hal_ready "$next_hal" "$stopped_hal" && owned_payload_hal "$next_hal";then
   if [ "$owned_hal_mode" = normal ];then
    if profile_latency_normal_ready "$next_hal" "$own_old_hal";then owned_ready=1;break;fi
   else owned_ready=1;break;fi
  fi
  sleep 0.1;owned_attempt=$((owned_attempt+1))
 done
 [ "$owned_ready" = 1 ] || owned_fail 'full new HAL readiness unavailable'
 record_process hal "$next_hal" || owned_fail 'cannot bind new HAL generation'
 owned_bt_same || owned_fail 'HAL-only recreation changed Bluetooth generation'
}
owned_to_normal(){
 owned_force=$1;owned_had_runtime=0
 if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ] || [ -e "$STATE/latency-remote-uncertain" ] || [ -L "$STATE/latency-remote-uncertain" ];then owned_had_runtime=1;fi
 owned_step
 # Preserve the old health token/watcher until final readiness. Delayed health
 # rollback rechecks its generation after FD9 releases; an orphan still has a
 # live independent owner able to recover it.
 if [ "$owned_had_runtime" = 1 ];then
  profile_stop_runtime || owned_fail 'runtime lease revocation failed'
 elif [ -e "$STATE/latency-provider.pid" ] || [ -e "$STATE/latency-provider.start" ] || [ -L "$STATE/latency-provider.pid" ] || [ -L "$STATE/latency-provider.start" ];then
  owned_fail 'publisher metadata has no verified current runtime owner'
 fi
 owned_clear_request
 owned_unmount_gsl
 if [ "$owned_had_runtime" = 1 ];then
  owned_runtime recover-dsp || owned_fail 'remote ownership recovery remains unresolved'
  owned_start_hal normal
  profile_finish_runtime_recovery "$next_hal" || owned_fail 'normal DSP/HAL finalization is unconfirmed'
 elif [ "$owned_force" = 1 ];then
  candidate_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  candidate_ticks=$(process_start "$candidate_hal") || candidate_ticks=''
  if [ -n "$candidate_ticks" ] && { [ "$candidate_hal" != "$own_old_hal" ] || [ "$candidate_ticks" != "$own_old_start" ]; } && profile_latency_normal_ready "$candidate_hal" "$own_old_hal" && owned_payload_hal "$candidate_hal";then
   next_hal=$candidate_hal;record_process hal "$next_hal" || owned_fail 'cannot bind existing recovered HAL'
  else owned_start_hal normal;fi
 else
  candidate_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
  profile_latency_normal_ready "$candidate_hal" 0 && owned_payload_hal "$candidate_hal" || owned_fail 'initial normal HAL is not verified'
 fi
 owned_step
}
owned_step
case "$owned_action" in
 1)
  [ ! -e "$STATE/armed" ] && [ ! -L "$STATE/armed" ] || owned_fail 'finish the existing core trial before tuning'
  [ ! -e "$STATE/dsp-trial.active" ] && [ ! -L "$STATE/dsp-trial.active" ] && [ ! -e "$STATE/dsp-trial.request" ] && [ ! -L "$STATE/dsp-trial.request" ] || owned_fail 'another DSP experiment owns this session'
  owned_receipt || exit 20
  ;;
 2)
  owned_read_owner;owned_to_normal 0
  owned_call owned_runtime request-owned || owned_fail 'owned current-boot request preparation failed'
  owned_runtime mount || owned_fail 'owned runtime mount failed'
  owned_start_hal runtime
  profile_start_runtime || owned_fail 'new root provider did not start'
  profile_verify_runtime "$next_hal" || owned_fail 'new runtime identity mismatch'
  owned_health || owned_fail 'new health generation is unconfirmed'
  owned_receipt || exit 30
  ;;
 3|5)
  owned_read_owner;owned_to_normal 1
  if [ -f "$STATE/active" ] && [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ];then owned_health || owned_fail 'normal health generation is unconfirmed';fi
  owned_receipt || exit 30
  ;;
 4) owned_receipt || exit 30;;
 *) owned_fail 'unreviewed fixed action';;
esac
