#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
/data/adb/ksu/bin/busybox sh "$MODDIR/control.sh" rollback
