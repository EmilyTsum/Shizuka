#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only; no policy writes, codec changes, mounts or logging activation.
set -eu
MODDIR=${0%/*}
export CLASSPATH="$MODDIR/status.dex"
exec /data/adb/ksu/bin/busybox timeout 7 app_process /system/bin BtStatus
