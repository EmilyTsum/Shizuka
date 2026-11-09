#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
exec "$BB" sh "$MODDIR/service.sh"
