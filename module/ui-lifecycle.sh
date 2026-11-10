#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Session-local UI ownership: pending removal is handled before temporary root disappears.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/runtime.sh"
ensure_busybox_shell "$@"
mkdir -p "$STATE"
stop_worker() {
 if [ -f "$STATE/ui-owner.pid" ] && [ -f "$STATE/ui-owner.start" ];then
  pid=$(cat "$STATE/ui-owner.pid");start=$(cat "$STATE/ui-owner.start")
  if process_matches "$pid" "$start" && "$BB" tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F "$MODDIR/ui-lifecycle.sh" >/dev/null;then
   kill -TERM "$pid" 2>/dev/null || true
  fi
 fi
 rm -f "$STATE/ui-owner.pid" "$STATE/ui-owner.start"
}
case "${1:-start}" in
 start)
  # Installer staging directories are not live module sessions.
  [ "$MODDIR" = /data/adb/modules/s25_aptx_adaptive ] || exit 0
  if [ -f "$STATE/ui-owner.pid" ] && [ -f "$STATE/ui-owner.start" ] && saved_process_matches ui-owner "$(cat "$STATE/ui-owner.pid")";then exit 0;fi
  "$BB" setsid "$BB" sh "$MODDIR/ui-lifecycle.sh" watch 9>&- > "$STATE/ui-owner.log" 2>&1 < /dev/null &
  record_process ui-owner "$!"
  ;;
 stop) stop_worker;;
 watch)
  boot=$(cat /proc/sys/kernel/random/boot_id)
  while [ "$(cat /proc/sys/kernel/random/boot_id)" = "$boot" ];do
   if [ -e "$MODDIR/remove" ] || [ ! -d "$MODDIR" ];then
    if [ -f "$MODDIR/control.sh" ];then "$BB" sh "$MODDIR/control.sh" rollback || true;fi
    if [ -f "$MODDIR/settings-bridge.sh" ];then "$BB" sh "$MODDIR/settings-bridge.sh" stop || true;fi
    # A transient Package Manager failure is not proof that the APK is gone.
    if packages=$(pm list packages dev.shizuka.settings 2>/dev/null);then
     if printf '%s\n' "$packages" | grep -Fxq package:dev.shizuka.settings;then
      pm uninstall dev.shizuka.settings || true
     else
      exit 0
     fi
    fi
   fi
   if [ -e "$MODDIR/disable" ] && [ -f "$MODDIR/settings-bridge.sh" ];then
    "$BB" sh "$MODDIR/settings-bridge.sh" stop || true
    if [ -f "$STATE/active" ];then "$BB" sh "$MODDIR/control.sh" rollback || true;fi
   fi
   sleep 2
  done
  ;;
 *) exit 64;;
esac
