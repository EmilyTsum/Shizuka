#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# KernelSU Action opens the native settings app without changing audio routing.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
exec "$BB" sh "$MODDIR/settings.sh"
