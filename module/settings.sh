#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The companion uses the same root CLI as external tools; launching never applies patches.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
PACKAGE=dev.shizuka.settings
ACTIVITY=dev.shizuka.settings.MainActivity
[ "$(id -u)" = 0 ] || { echo 'Root is required to open Shizuka from KernelSU.' >&2;exit 1; }
[ -f "$MODDIR/settings.apk" ] && [ -f "$MODDIR/settings.sha256" ] || {
 echo 'Shizuka settings APK is missing. Reinstall the complete module.' >&2;exit 1;
}
[ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || { echo 'Shizuka module is disabled or pending removal.' >&2;exit 1; }
expected=$(cat "$MODDIR/settings.sha256")
actual=$("$BB" sha256sum "$MODDIR/settings.apk");actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'Shizuka settings APK hash mismatch.' >&2;exit 1; }
installed=$(pm path "$PACKAGE" 2>/dev/null | "$BB" sed -n 's/^package://p' | "$BB" head -n 1)
installed_hash=''
if [ -n "$installed" ] && [ -f "$installed" ];then
 installed_hash=$("$BB" sha256sum "$installed");installed_hash=${installed_hash%% *}
fi
"$BB" sh "$MODDIR/ui-lifecycle.sh" start
if [ "$installed_hash" != "$expected" ];then
 # Package Manager consumes stdin so it need not traverse private /data/adb directories.
 size=$("$BB" stat -c '%s' "$MODDIR/settings.apk")
 pm install -r -S "$size" < "$MODDIR/settings.apk"
 installed=$(pm path "$PACKAGE" | "$BB" sed -n 's/^package://p' | "$BB" head -n 1)
 [ -n "$installed" ] && [ -f "$installed" ] || exit 1
 installed_hash=$("$BB" sha256sum "$installed");installed_hash=${installed_hash%% *}
 [ "$installed_hash" = "$expected" ] || { echo 'Installed Shizuka APK differs from the module.' >&2;exit 1; }
fi
if [ "${1:-}" = --install-only ];then exit 0;fi
current_user=$(am get-current-user)
companion_uid=$(cmd package list packages -U --user "$current_user" "$PACKAGE" | "$BB" awk -v package="package:$PACKAGE" '$1==package && $2 ~ /^uid:/ {sub(/^uid:/,"",$2);print $2}')
case "$companion_uid" in ''|*[!0-9]*) echo 'Internal UI UID is unavailable.' >&2;exit 1;;esac
socket=$("$BB" sh "$MODDIR/settings-bridge.sh" start "$companion_uid")
case "$socket" in shizuka-settings-*) ;;*) echo 'Shizuka Root session could not start.' >&2;exit 1;;esac
exec am start --user current -n "$PACKAGE/$ACTIVITY" --es shizuka_socket "$socket"
