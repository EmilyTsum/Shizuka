#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=$1
watched=$2
started=$3
token=$4
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/runtime.sh"
while [ -f "$STATE/active" ] && token_matches "$STATE/health-token" "$token";do
 hal_alive=true
 if [ -f "$STATE/hal.pid" ] && [ -f "$STATE/hal.start" ];then
  process_matches "$(cat "$STATE/hal.pid")" "$(cat "$STATE/hal.start")" || hal_alive=false
 fi
 runtime_alive=true
 if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ];then
  if ! "$BB" sh "$MODDIR/latency-runtime.sh" health;then runtime_alive=false;fi
 fi
 application_alive=true
 if ! application_fence health > "$STATE/latency-application-health.json" 2>&1;then application_alive=false;fi
 # A bounded reader may span a completed owner rotation. Do not send a stale
 # recovery request after the current generation has already been replaced.
 [ -f "$STATE/active" ] && token_matches "$STATE/health-token" "$token" || exit 0
 if ! process_matches "$watched" "$started" || [ "$hal_alive" != true ] || [ "$runtime_alive" != true ] || [ "$application_alive" != true ];then
  if [ -n "$(getprop sys.shutdown.requested)" ];then
   echo 'System shutdown requested; ending health watcher'
   exit 0
  fi
  echo 'Bluetooth, audio HAL, latency runtime or delay operation health failed; requesting recovery for this generation'
  "$BB" sh "$MODDIR/control.sh" rollback-health "$token"
  exit 0
 fi
 sleep 2
done
