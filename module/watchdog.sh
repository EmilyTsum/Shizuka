#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=$1
seconds=$2
token=$3
BB=/data/adb/ksu/bin/busybox
elapsed=0
while [ "$elapsed" -lt "$seconds" ];do
 [ -f "$MODDIR/state/armed" ] && [ "$(cat "$MODDIR/state/armed")" = "$token" ] || exit 0
 if [ -f "$MODDIR/state/active" ] && [ -f "$MODDIR/state/bt.pid" ];then
  watched=$(cat "$MODDIR/state/bt.pid")
  if ! kill -0 "$watched" 2>/dev/null;then
   echo 'Bluetooth process exited; rolling back immediately'
   "$BB" sh "$MODDIR/control.sh" rollback;exit 0
  fi
 fi
 sleep 1;elapsed=$((elapsed+1))
done
[ -f "$MODDIR/state/armed" ] && [ "$(cat "$MODDIR/state/armed")" = "$token" ] || exit 0
"$BB" sh "$MODDIR/control.sh" rollback
