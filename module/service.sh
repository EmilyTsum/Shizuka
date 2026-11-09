#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
mkdir -p "$MODDIR/state"
if [ -f "$MODDIR/ui-lifecycle.sh" ];then "$BB" sh "$MODDIR/ui-lifecycle.sh" start;fi
if [ -x "$MODDIR/dsp-capture" ] && [ -f "$MODDIR/dsp-capture.sha256" ]; then
 actual=$("$BB" sha256sum "$MODDIR/dsp-capture");actual=${actual%% *}
 if [ "$actual" = "$(cat "$MODDIR/dsp-capture.sha256")" ]; then
  "$MODDIR/dsp-capture" --recover > "$MODDIR/state/diagnostics-recovery.log" 2>&1
 fi
fi
"$BB" setsid "$BB" sh "$MODDIR/boot-activate.sh" > "$MODDIR/state/boot-activate.log" 2>&1 < /dev/null &
exit 0
