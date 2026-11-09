#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# One-shot Android codec preferences. No daemon, DSP mount or service restart.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
[ "$(id -u)" = 0 ] || { echo 'root required' >&2; exit 1; }
expected=$(cat "$MODDIR/policy.sha256")
actual=$("$BB" sha256sum "$MODDIR/policy.dex"); actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'policy.dex hash mismatch' >&2; exit 1; }
export CLASSPATH="$MODDIR/policy.dex"
exec "$BB" timeout 7 app_process /system/bin BtPlayback "$@"
