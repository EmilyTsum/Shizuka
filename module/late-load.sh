#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
mkdir -p "$MODDIR/state"
"$BB" setsid "$BB" sh "$MODDIR/boot-activate.sh" > "$MODDIR/state/boot-activate.log" 2>&1 < /dev/null &
exit 0
