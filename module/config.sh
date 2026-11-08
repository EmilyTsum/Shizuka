#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
CONFIG=/data/adb/s25_bluetooth/config.conf
BB=/data/adb/ksu/bin/busybox
[ "$(id -u)" = 0 ] || { echo 'root required' >&2;exit 1; }
if [ "$#" = 0 ];then set -- get;fi
case "${1:-get}" in
 stop)
  if [ -f "$MODDIR/state/policy.pid" ];then
   pid=$(cat "$MODDIR/state/policy.pid")
   if [ -r "/proc/$pid/cmdline" ] && "$BB" tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F 'BtConfig' >/dev/null;then kill -TERM "$pid";fi
   rm -f "$MODDIR/state/policy.pid"
  fi;exit 0;;
esac
[ -f "$CONFIG" ] || { echo "Missing $CONFIG; copy config.example.conf and set target_address" >&2;exit 1; }
expected=$(cat "$MODDIR/policy.sha256")
actual=$("$BB" sha256sum "$MODDIR/policy.dex");actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'policy.dex hash mismatch' >&2;exit 1; }
export CLASSPATH="$MODDIR/policy.dex"
case "${1:-get}" in
 watch)
  [ -f "$MODDIR/state/active" ] || { echo 'module not active' >&2;exit 1; }
  exec app_process /system/bin BtConfig "$CONFIG" watch;;
 start)
  app_process /system/bin BtConfig "$CONFIG" validate >/dev/null
  mkdir -p "$MODDIR/state"
  if [ -f "$MODDIR/state/policy.pid" ];then
   pid=$(cat "$MODDIR/state/policy.pid")
   if [ -r "/proc/$pid/cmdline" ] && "$BB" tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F 'BtConfig' >/dev/null;then exit 0;fi
  fi
  "$BB" setsid "$BB" sh "$0" watch > "$MODDIR/state/policy.log" 2>&1 < /dev/null &
  echo $! > "$MODDIR/state/policy.pid";;
 get|value|validate|set|apply|apply-transport|apply-adaptive|apply-permissions)
  exec app_process /system/bin BtConfig "$CONFIG" "$@";;
 *) echo 'usage: config.sh get|validate|set KEY VALUE|apply|apply-transport|apply-adaptive|start|stop' >&2;exit 1;;
esac
