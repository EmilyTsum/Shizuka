#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Normal app entry: authorize the installed companion and open only its root bridge.
set -eu
MODDIR=/data/adb/modules/s25_aptx_adaptive
BB=/data/adb/ksu/bin/busybox
PACKAGE=dev.shizuka.settings
fail() {
 printf '{"schema":1,"scope":"settings_session","available":false,"code":"%s","reason":"%s"}\n' "$1" "$2"
 exit 1
}
[ "$(id -u)" = 0 ] || fail root_unavailable 'Root access is required. Allow Shizuka in KernelSU, then retry.'
[ -x "$BB" ] || fail root_unavailable 'KernelSU root tools are unavailable. Restore root access, then retry.'
[ "$#" = 1 ] || fail invalid_request 'The companion UID is required.'
uid=$1
case "$uid" in ''|0*|*[!0-9]*) fail invalid_request 'The companion UID is invalid.';;esac
[ "${#uid}" -le 10 ] && [ "$uid" -ge 10000 ] && [ "$uid" -le 2147483647 ] || fail invalid_request 'The companion UID is invalid.'
[ -d "$MODDIR" ] && [ ! -L "$MODDIR" ] && [ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || fail module_unavailable 'Install the Shizuka KernelSU module, then retry.'
for marker in disable remove;do
 [ ! -e "$MODDIR/$marker" ] && [ ! -L "$MODDIR/$marker" ] || fail module_unavailable 'Enable the Shizuka module and cancel pending removal, then retry.'
done
trusted_file() {
 [ -f "$1" ] && [ ! -L "$1" ] || return 1
 info=$("$BB" stat -c '%u:%a:%h' "$1") || return 1
 owner=${info%%:*};rest=${info#*:};mode=${rest%%:*};links=${rest##*:}
 case "$mode" in ''|*[!0-7]*) return 1;;esac
 [ "$owner" = 0 ] && [ "$links" = 1 ] && [ "$((0$mode & 022))" = 0 ]
}
for file in module.prop settings.apk settings.sha256 settings-bridge.sh ui-lifecycle.sh;do
 trusted_file "$MODDIR/$file" || fail module_unavailable 'The installed module is incomplete or has unsafe entry files.'
done
"$BB" grep -Fxq 'id=s25_aptx_adaptive' "$MODDIR/module.prop" || fail module_unavailable 'The installed module identity does not match Shizuka.'
expected=$(cat "$MODDIR/settings.sha256")
[ "${#expected}" = 64 ] || fail app_mismatch 'The module settings identity is invalid. Reinstall the complete module.'
printf '%s\n' "$expected" | "$BB" grep -Eq '^[0-9a-f]{64}$' || fail app_mismatch 'The module settings identity is invalid. Reinstall the complete module.'
actual=$("$BB" sha256sum "$MODDIR/settings.apk");actual=${actual%% *}
[ "$actual" = "$expected" ] || fail app_mismatch 'The module settings APK does not match its recorded identity.'
user=$(am get-current-user)
case "$user" in ''|*[!0-9]*) fail uid_mismatch 'The current Android user is unavailable.';;esac
actual_uid=$(cmd package list packages -U --user "$user" "$PACKAGE" | "$BB" awk -v package="package:$PACKAGE" '$1==package && $2 ~ /^uid:/ {sub(/^uid:/,"",$2);print $2}')
[ "$actual_uid" = "$uid" ] || fail uid_mismatch 'This app does not match the installed Shizuka companion for the current user.'
installed=$(pm path --user "$user" "$PACKAGE" | "$BB" sed -n 's/^package://p' | "$BB" head -n 1)
[ -n "$installed" ] && [ -f "$installed" ] && [ ! -L "$installed" ] || fail app_mismatch 'The installed Shizuka APK is unavailable. Reinstall the complete module.'
actual=$("$BB" sha256sum "$installed");actual=${actual%% *}
[ "$actual" = "$expected" ] || fail app_mismatch 'Update the Shizuka app and module to matching versions, then retry.'
"$BB" sh "$MODDIR/ui-lifecycle.sh" start >/dev/null || fail bridge_unavailable 'The module session owner could not start. Retry the connection.'
socket=$("$BB" sh "$MODDIR/settings-bridge.sh" start "$uid") || fail bridge_unavailable 'The module connection could not start. Retry or inspect module diagnostics.'
[ "${#socket}" = 53 ] || fail bridge_unavailable 'The module returned an invalid connection identity.'
printf '%s\n' "$socket" | "$BB" grep -Eq '^shizuka-settings-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' || fail bridge_unavailable 'The module returned an invalid connection identity.'
printf '{"schema":1,"scope":"settings_session","available":true,"companion_uid":%s,"socket":"%s"}\n' "$uid" "$socket"
