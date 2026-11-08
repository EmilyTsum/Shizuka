#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=$1
watched=$2
started=$3
token=$4
BB=/data/adb/ksu/bin/busybox
while [ -f "$MODDIR/state/active" ] && [ -f "$MODDIR/state/health-token" ] && [ "$(cat "$MODDIR/state/health-token")" = "$token" ];do
 actual=''
 if [ -r "/proc/$watched/stat" ];then actual=$("$BB" awk '{print $22}' "/proc/$watched/stat");fi
 if [ "$actual" != "$started" ];then
  if [ -n "$(getprop sys.shutdown.requested)" ];then
   echo 'System shutdown requested; ending health watcher'
   exit 0
  fi
  echo 'Verified Bluetooth process exited; restoring stock and disabling automatic re-entry'
  touch "$MODDIR/.no-auto"
  "$BB" sh "$MODDIR/control.sh" rollback
  exit 0
 fi
 sleep 2
done
