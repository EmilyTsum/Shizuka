#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
result=0
if [ -f "$MODDIR/ui-lifecycle.sh" ];then "$BB" sh "$MODDIR/ui-lifecycle.sh" stop || result=$?;fi
"$BB" sh "$MODDIR/control.sh" rollback || result=$?
if [ -f "$MODDIR/settings-bridge.sh" ];then "$BB" sh "$MODDIR/settings-bridge.sh" stop || result=$?;fi
if pm path dev.shizuka.settings >/dev/null 2>&1;then
 pm uninstall dev.shizuka.settings || result=$?
fi
exit "$result"
