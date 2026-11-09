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
 if ! process_matches "$watched" "$started";then
  if [ -n "$(getprop sys.shutdown.requested)" ];then
   echo 'System shutdown requested; ending health watcher'
   exit 0
  fi
  echo 'Verified Bluetooth process exited; requesting recovery for this generation'
  "$BB" sh "$MODDIR/control.sh" rollback-health "$token"
  exit 0
 fi
 sleep 2
done
