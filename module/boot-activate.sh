#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# One attempt per stable host framework session, only for a heard/verified payload.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/runtime.sh"
mkdir -p "$STATE"
[ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] && [ ! -f "$MODDIR/.no-auto" ] || exit 0
[ -f "$MODDIR/proofs/verified-payloads.sha256" ] || { echo "Audio verification pending; use KernelSU Action for a bounded trial";exit 0; }
"$BB" cmp -s "$MODDIR/payloads.sha256" "$MODDIR/proofs/verified-payloads.sha256" || exit 1
boot=$(cat /proc/sys/kernel/random/boot_id)
lock="$STATE/boot-worker.$boot"
if ! mkdir "$lock" 2>/dev/null;then
 if [ -s "$lock/pid" ] && ! kill -0 "$(cat "$lock/pid")" 2>/dev/null;then
  rm -f "$lock/pid";rmdir "$lock" || exit 0;mkdir "$lock" || exit 0
 else exit 0;fi
fi
echo $$ > "$lock/pid"
trap 'rm -f "$lock/pid";rmdir "$lock" 2>/dev/null || true' EXIT
# DFRoot can call late-load before its soft reboot. Wait for a stable framework.
last='';stable=0;elapsed=0;ready=0
while [ "$elapsed" -lt 180 ];do
 [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] && [ ! -f "$MODDIR/.no-auto" ] || exit 0
 z=$(host_zygote);bt=$(host_bt)
 if [ "$(getprop sys.boot_completed)" = 1 ] && [ -n "$z" ] && [ -n "$bt" ] && start=$(process_start "$z");then
  session="$boot:$z:$start"
  if [ "$session" = "$last" ];then stable=$((stable+5));else last=$session;stable=0;fi
  if [ "$stable" -ge 20 ];then ready=1;break;fi
 else stable=0;last='';fi
 sleep 5;elapsed=$((elapsed+5))
done
[ "$ready" = 1 ] || { echo 'No stable Bluetooth-enabled host session; skipped';exit 0; }
if [ -f "$STATE/active" ] && [ -f "$STATE/bt.pid" ] && [ "$(cat "$STATE/bt.pid")" = "$bt" ];then
 if "$BB" sh "$MODDIR/control.sh" keep-verified;then exit 0;fi
fi
if [ "${1:-}" != manual ] && [ -f "$STATE/boot-attempt" ] && [ "$(cat "$STATE/boot-attempt")" = "$session" ];then
 echo 'Already attempted this framework session; skipped';exit 0
fi
echo "$session" > "$STATE/boot-attempt"
if [ -f "$STATE/active" ] || [ -f "$STATE/armed" ];then
 "$BB" sh "$MODDIR/control.sh" rollback
 sleep 8
fi
"$BB" sh "$MODDIR/control.sh" preflight
"$BB" sh "$MODDIR/control.sh" activate
"$BB" sh "$MODDIR/control.sh" keep-verified
