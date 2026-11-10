#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
fail() { echo "Shizuka service: $*" >&2;exit 1; }
. "$MODDIR/lib/runtime.sh"
ensure_busybox_shell "$@"
prepare_state
if [ -f "$MODDIR/ui-lifecycle.sh" ];then "$BB" sh "$MODDIR/ui-lifecycle.sh" start;fi
if [ "$MODDIR" = /data/adb/modules/s25_aptx_adaptive ] && \
 [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] && [ -f "$MODDIR/settings.sh" ];then
 # Register the ordinary launcher app from the final module directory.
 # Package Manager may be briefly unavailable during late-load; its installer
 # cannot hold boot or an inherited lifecycle lock indefinitely.
 "$BB" setsid "$BB" timeout -s TERM -k 5 60 "$BB" sh "$MODDIR/settings.sh" --install-only \
  9>&- 8>&- > "$STATE/settings-install.log" 2>&1 < /dev/null &
fi
if [ -x "$MODDIR/dsp-capture" ] && [ -f "$MODDIR/dsp-capture.sha256" ]; then
 actual=$("$BB" sha256sum "$MODDIR/dsp-capture");actual=${actual%% *}
 if [ "$actual" = "$(cat "$MODDIR/dsp-capture.sha256")" ]; then
  "$MODDIR/dsp-capture" --recover > "$MODDIR/state/diagnostics-recovery.log" 2>&1
 fi
fi
"$BB" setsid "$BB" sh "$MODDIR/boot-activate.sh" > "$MODDIR/state/boot-activate.log" 2>&1 < /dev/null &
exit 0
