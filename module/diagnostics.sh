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
 capture|recover)
  if [ "$1" = capture ]; then
   [ "$#" = 2 ] || { echo 'usage: diagnostics.sh capture SECONDS (5..120)' >&2;exit 64; }
  else
   [ "$#" = 1 ] || exit 64
  fi
  [ -f "$MODDIR/device-profile" ] && [ "$(cat "$MODDIR/device-profile")" = samsung-sm-s931z-dzif ] || { echo 'DSP diagnostics require the verified DZIF backend' >&2;exit 1; }
  [ "$(getprop ro.build.version.incremental)" = S931ZSCU1DZIF ] || { echo 'unsupported firmware' >&2;exit 1; }
  expected=$(cat "$MODDIR/dsp-capture.sha256")
  actual=$("$BB" sha256sum "$MODDIR/dsp-capture");actual=${actual%% *}
  [ "$actual" = "$expected" ] || { echo 'dsp-capture hash mismatch' >&2;exit 1; }
  [ "$1" != recover ] || exec "$MODDIR/dsp-capture" --recover
  actual=$("$BB" sha256sum /vendor/dsp/adsp/aptx_adaptive_enc_module.so.1);actual=${actual%% *}
  case "$actual" in
   05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3|ba24759a991e20b0a775b5cb51b8707155f45118ac15a7b73582736dfb858089|bfe204dd8b90f698df1c6107f5f78b300757619dad72e5033e4cf318490289a9) ;;
   ee628997be7f1b6d2b74899a25a4c9715e62dd495e87f75881337ec5aa5efaf8|df149eaacf731ad5a27b487a8aa2a27b10d99983e305a737c511e6b4369f8efc|16910bf1de410f38e84c889f6f424412be482b339509659084284bbb95fe992f|4c1fae4a0eb5bafbda0b19da5f5e5067fc3736f0b35df9d5a57ea5b9705f5dfe|4e7838e2ba87f0968432b8826469c29a9847caa2d231d69ac67487fe3c6bfb8f|11170588cfee87f800deee56789286ddeffbf9e0d597914e09e71cd434fb719f) ;;
   *) echo 'unrecognized DSP image; diagnostics refused' >&2;exit 1;;
  esac;;
 list|export|help) ;;
 *) echo 'usage: diagnostics.sh capture SECONDS|list|export SESSION|recover|help' >&2;exit 64;;
esac
export CLASSPATH="$MODDIR/policy.dex"
exec app_process /system/bin BtDiagnostics "$MODDIR" "$@"
