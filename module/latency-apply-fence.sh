#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only check/health; reconcile only finalizes verified normal-owner metadata.
set -eu
MODDIR=${0%/*};STATE="$MODDIR/state";BB=/data/adb/ksu/bin/busybox
[ "$#" = 1 ] || exit 3
case "$1" in check|health|reconcile) ;;*) exit 3;;esac
fence_unknown(){ printf '%s\n' '{"schema":1,"scope":"latency_application_fence","known":false,"unresolved":true,"normal_mutation_allowed":false,"healthy":false,"orphaned":true,"reason":"Application ownership has not been reconciled"}';exit 3; }
[ -x "$BB" ] && [ "$(id -u)" = 0 ] && [ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || fence_unknown
[ -d "$STATE" ] && [ ! -L "$STATE" ] && [ "$("$BB" stat -c '%u:%a' "$STATE")" = 0:700 ] || fence_unknown
if [ "$1" = reconcile ];then
 # Only confirm the inherited descriptor, never reopen/relock in a child.
 [ -f "$STATE/lifecycle.lock" ] && [ ! -L "$STATE/lifecycle.lock" ] || fence_unknown
 inherited_lock=$("$BB" stat -Lc '%d:%i' /proc/self/fd/9) || fence_unknown
 expected_lock=$("$BB" stat -c '%d:%i' "$STATE/lifecycle.lock") || fence_unknown
 [ -n "$inherited_lock" ] && [ "$inherited_lock" = "$expected_lock" ] || fence_unknown
 "$BB" flock -n 9 || fence_unknown
fi
directory="$STATE/latency-application"
if [ ! -e "$directory" ] && [ ! -L "$directory" ];then
 printf '%s\n' '{"schema":1,"scope":"latency_application_fence","known":true,"unresolved":false,"normal_mutation_allowed":true,"healthy":true,"orphaned":false,"current_operation_id":null,"reason":"No application ownership journal exists"}'
 exit 0
fi
[ -d "$directory" ] && [ ! -L "$directory" ] && [ "$("$BB" stat -c '%u:%a' "$directory")" = 0:700 ] || fence_unknown
fence_trusted(){ [ -f "$1" ] && [ ! -L "$1" ] || return 1;fence_a=$("$BB" stat -c '%u:%a:%h' "$1") || return 1;case "$fence_a" in 0:*:1) ;;*) return 1;;esac;fence_p=${fence_a#0:};fence_p=${fence_p%:1};case "$fence_p" in ''|*[!0-7]*) return 1;;esac;[ "$((0$fence_p & 022))" = 0 ]; }
if fence_trusted "$MODDIR/latency-application.dex" && fence_trusted "$MODDIR/latency-application.sha256";then
 [ "$("$BB" stat -c '%s' "$MODDIR/latency-application.sha256")" = 65 ] || fence_unknown
 digest=$("$BB" sha256sum "$MODDIR/latency-application.dex");digest=${digest%% *}
 [ "$digest" = "$(cat "$MODDIR/latency-application.sha256")" ] || fence_unknown
 export CLASSPATH="$MODDIR/latency-application.dex"
 if [ "$1" = reconcile ];then exec /system/bin/app_process /system/bin LatencyApplyReconcileMain --module-dir "$MODDIR";fi
 exec /system/bin/app_process /system/bin LatencyApplyFenceMain --module-dir "$MODDIR" "$1"
fi
# An absent optional DEX is clear only for a genuinely empty protected directory.
# Any record, owner, channel or unknown object remains fenced; no record is deleted.
if [ -z "$("$BB" find "$directory" -mindepth 1 -maxdepth 1 -print -quit)" ];then
 printf '%s\n' '{"schema":1,"scope":"latency_application_fence","known":true,"unresolved":false,"normal_mutation_allowed":true,"healthy":true,"orphaned":false,"current_operation_id":null,"reason":"The protected application journal is empty"}'
 exit 0
fi
fence_unknown
