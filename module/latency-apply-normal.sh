#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Independent read-only full STOCK baseline receipt. No service/codec/play writes.
set -eu
MODDIR=${0%/*};STATE="$MODDIR/state";BB=/data/adb/ksu/bin/busybox
normal_fail(){ echo 'Verified stock normal audio evidence unavailable' >&2;exit 3; }
[ "$#" = 0 ] && [ "$(id -u)" = 0 ] && [ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || normal_fail
normal_trusted(){ [ -f "$1" ] && [ ! -L "$1" ] || normal_fail;a=$("$BB" stat -c '%u:%a:%h' "$1");case "$a" in 0:*:1) ;;*) normal_fail;;esac;p=${a#0:};p=${p%:1};case "$p" in ''|*[!0-7]*) normal_fail;;esac;[ "$((0$p & 022))" = 0 ] || normal_fail; }
for f in "$MODDIR/lib/runtime.sh" "$MODDIR/lib/device.sh" "$MODDIR/device-profile" "$MODDIR/profiles/samsung-sm-s931z-dzif.sh" "$MODDIR/inputs.sha256" "$MODDIR/latency-application.dex" "$MODDIR/latency-application.sha256";do normal_trusted "$f";done
[ "$(cat "$MODDIR/device-profile")" = samsung-sm-s931z-dzif ] || normal_fail
. "$MODDIR/lib/runtime.sh";fail(){ normal_fail; };ensure_busybox_shell "$@"
. "$MODDIR/lib/device.sh"
[ "$(sha "$MODDIR/latency-application.dex")" = "$(cat "$MODDIR/latency-application.sha256")" ] || normal_fail
[ -d "$STATE" ] && [ ! -L "$STATE" ] && [ "$("$BB" stat -c '%u:%a' "$STATE")" = 0:700 ] || normal_fail
for marker in latency-runtime.active latency-remote-uncertain;do [ ! -e "$STATE/$marker" ] && [ ! -L "$STATE/$marker" ] || normal_fail;done
# The pending request is metadata only. The reconciler validates its ownership
# and archives it after this independent physical proof, before resolving rows.
for marker in latency-provider.pid latency-provider.start;do [ ! -e "$STATE/$marker" ] && [ ! -L "$STATE/$marker" ] || normal_fail;done
normal_boot=$(cat /proc/sys/kernel/random/boot_id)
printf '%s\n' "$normal_boot" | "$BB" grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' || normal_fail
normal_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE");normal_start=$(process_start "$normal_hal") || normal_fail
profile_latency_recovery_preflight >/dev/null 2>&1 && profile_latency_normal_ready "$normal_hal" 0 >/dev/null 2>&1 || normal_fail
normal_z=$(host_zygote);[ -n "$normal_z" ] || normal_fail
normal_bt=$(host_bt || true);normal_count=0;normal_seen=' '
while IFS=' ' read -r expected target extra;do
 [ -n "$expected" ] || continue
 [ -z "${extra:-}" ] && [ "${#expected}" = 64 ] || normal_fail
 case "$expected" in *[!0-9a-f]*) normal_fail;;esac
 profile_target_allowed "$target" || normal_fail
 case "$normal_seen" in *" $target "*) normal_fail;;esac
 normal_seen="$normal_seen$target ";normal_count=$((normal_count+1))
 actual=$(nsrun 1 "$BB" sha256sum "$target");[ "${actual%% *}" = "$expected" ] || normal_fail
 case "$target" in
  "$NATIVE"|"$APK")
   actual=$(nsrun "$normal_z" "$BB" sha256sum "$target");[ "${actual%% *}" = "$expected" ] || normal_fail
   if [ -n "$normal_bt" ];then actual=$(nsrun "$normal_bt" "$BB" sha256sum "$target");[ "${actual%% *}" = "$expected" ] || normal_fail;fi
   ;;
  "$HAL") actual=$(nsrun "$normal_hal" "$BB" sha256sum "$target");[ "${actual%% *}" = "$expected" ] || normal_fail;;
 esac
done < "$MODDIR/inputs.sha256"
[ "$normal_count" = "$STOCK_TARGET_COUNT" ] || normal_fail
[ "$normal_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] && [ "$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")" = "$normal_hal" ] && process_matches "$normal_hal" "$normal_start" || normal_fail
normal_now=$(CLASSPATH="$MODDIR/latency-application.dex" app_process /system/bin LatencyMonotonicMain) || normal_fail
case "$normal_now" in ''|*[!0-9]*) normal_fail;;esac
printf '{"schema":1,"scope":"application_stock_normal","stock_normal_verified":true,"boot_id":"%s","hal_pid":%s,"hal_start":"%s","monotonic_ms":%s,"inputs_sha256":"%s"}\n' "$normal_boot" "$normal_hal" "$normal_start" "$normal_now" "$(sha "$MODDIR/inputs.sha256")"
