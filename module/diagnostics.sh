#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
[ "$(id -u)" = 0 ] || { echo 'root required' >&2;exit 1; }
expected=$(cat "$MODDIR/policy.sha256")
actual=$("$BB" sha256sum "$MODDIR/policy.dex");actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'policy.dex hash mismatch' >&2;exit 1; }
[ "$#" -gt 0 ] || set -- help
case "${1:-help}" in
 capture|observe|recover)
  if [ "$1" = capture ]; then
   [ "$#" = 2 ] || { echo 'usage: diagnostics.sh capture SECONDS (5..120)' >&2;exit 64; }
  elif [ "$1" = observe ]; then
   [ "$#" = 2 ] || { echo 'usage: diagnostics.sh observe TOKEN' >&2;exit 64; }
  else
   [ "$#" = 1 ] || exit 64
  fi
  fail() { echo "$*" >&2;exit 1; }
  [ -f "$MODDIR/device-profile" ] || fail 'DSP diagnostics require a verified device backend'
  . "$MODDIR/lib/device.sh"
  command -v profile_diagnostics_preflight >/dev/null 2>&1 || fail 'DSP diagnostics unsupported by this backend'
  profile_diagnostics_preflight "$1"
  expected=$(cat "$MODDIR/dsp-capture.sha256")
  actual=$("$BB" sha256sum "$MODDIR/dsp-capture");actual=${actual%% *}
  [ "$actual" = "$expected" ] || { echo 'dsp-capture hash mismatch' >&2;exit 1; }
  [ "$1" != recover ] || exec "$MODDIR/dsp-capture" --recover
  ;;
 renew|stop) [ "$#" = 2 ] || exit 64 ;;
 list|export|help) ;;
 *) echo 'usage: diagnostics.sh observe TOKEN|renew TOKEN|stop TOKEN|capture SECONDS|list|export SESSION|recover|help' >&2;exit 64;;
esac
export CLASSPATH="$MODDIR/policy.dex"
exec app_process /system/bin BtDiagnostics "$MODDIR" "$@"
