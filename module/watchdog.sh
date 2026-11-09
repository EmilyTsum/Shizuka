#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=$1
seconds=$2
token=$3
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/runtime.sh"
elapsed=0
while [ "$elapsed" -lt "$seconds" ];do
 token_matches "$STATE/armed" "$token" || exit 0
 if [ -f "$STATE/active" ] && [ -f "$STATE/bt.pid" ];then
  watched=$(cat "$STATE/bt.pid")
  if ! saved_process_matches bt "$watched";then
   echo 'Bluetooth process exited; rolling back immediately'
   "$BB" sh "$MODDIR/control.sh" rollback-trial "$token";exit 0
  fi
 fi
 sleep 1;elapsed=$((elapsed+1))
done
token_matches "$STATE/armed" "$token" || exit 0
"$BB" sh "$MODDIR/control.sh" rollback-trial "$token"
