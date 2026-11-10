#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixed read-only application evidence. Never starts/stops a service or writes a graph.
set -eu
MODDIR=${0%/*};STATE="$MODDIR/state";BB=/data/adb/ksu/bin/busybox
fail(){ echo 'Shizuka application evidence unavailable' >&2;exit 1; }
[ "$(id -u)" = 0 ] && [ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || fail
trusted(){ [ -f "$1" ] && [ ! -L "$1" ] || fail;attributes=$("$BB" stat -c '%u:%a:%h' "$1");case "$attributes" in 0:*:1) ;;*) fail;;esac;permissions=${attributes#0:};permissions=${permissions%:1};case "$permissions" in ''|*[!0-7]*) fail;;esac;[ "$((0$permissions & 022))" = 0 ] || fail; }
for path in "$MODDIR/lib/runtime.sh" "$MODDIR/lib/device.sh" "$MODDIR/device-profile" "$MODDIR/profiles/samsung-sm-s931z-dzif.sh";do trusted "$path";done
[ "$(cat "$MODDIR/device-profile")" = samsung-sm-s931z-dzif ] || fail
. "$MODDIR/lib/runtime.sh";ensure_busybox_shell "$@"
. "$MODDIR/lib/device.sh"
[ -d "$STATE" ] && [ ! -L "$STATE" ] && [ "$("$BB" stat -c '%u:%a' "$STATE")" = 0:700 ] || fail
[ -f "$STATE/active" ] && [ ! -L "$STATE/active" ] && [ ! -e "$MODDIR/disable" ] && [ ! -L "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] && [ ! -L "$MODDIR/remove" ] || fail
[ ! -e "$STATE/latency-remote-uncertain" ] && [ ! -L "$STATE/latency-remote-uncertain" ] || fail
for path in "$STATE/bt.pid" "$STATE/bt.start" "$STATE/hal.pid" "$STATE/hal.start";do trusted "$path";done
read_bt=$(cat "$STATE/bt.pid");read_bt_start=$(cat "$STATE/bt.start")
[ "$(host_bt)" = "$read_bt" ] && process_matches "$read_bt" "$read_bt_start" || fail
read_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE");read_hal_start=$(process_start "$read_hal") || fail
[ "$(cat "$STATE/hal.pid")" = "$read_hal" ] && [ "$(cat "$STATE/hal.start")" = "$read_hal_start" ] || fail
for pair in "$read_bt:$NATIVE:$NATIVE_PAYLOAD" "$read_bt:$APK:$APK_PAYLOAD" "$read_hal:$HAL:$HAL_PAYLOAD";do
 read_pid=${pair%%:*};read_rest=${pair#*:};read_target=${read_rest%:*};read_payload=${read_rest##*:}
 trusted "$MODDIR/payload/$read_payload"
 actual=$(nsrun "$read_pid" "$BB" sha256sum "$read_target");[ "${actual%% *}" = "$(sha "$MODDIR/payload/$read_payload")" ] || fail
done
case "${1:-}" in
 preflight)
  [ "$#" = 1 ] || fail
  [ "$(getprop ro.build.version.incremental)" = S931ZSCU1DZIF ] && [ "$(cat /sys/class/remoteproc/remoteproc1/name)" = 3000000.remoteproc-adsp ] || fail
  [ -f /sys/kernel/boot_adsp/ssr ] && [ ! -L /sys/kernel/boot_adsp/ssr ] || fail
  for pair in /vendor_dlkm/lib/modules/adsp_loader_dlkm.ko:3d9708cbb7b5e5b64bbb0f4acea91a6ba4db9630d9ac8503f7dda2a546d8ec1c /vendor/dsp/adsp/aptx_adaptive_enc_module.so.1:05ec096c94ca717fd390c6bd8a37dd5f695f83c3f26e7e9f132c81b4f9f8ecc3;do
   actual=$(nsrun 1 "$BB" sha256sum "${pair%:*}");[ "${actual%% *}" = "${pair##*:}" ] || fail
  done
  hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE");ticks=$(process_start "$hal") || fail
  profile_hal_ready "$hal" 0 >/dev/null 2>&1 || fail
  if [ -e "$STATE/latency-runtime.active" ] || [ -L "$STATE/latency-runtime.active" ];then
   trusted "$MODDIR/latency-runtime.sh"
   "$BB" sh "$MODDIR/latency-runtime.sh" verify "$hal" >/dev/null 2>&1 || fail
   trusted "$MODDIR/latency-provider";trusted "$MODDIR/latency-provider.sha256"
   [ "$(sha "$MODDIR/latency-provider")" = "$(cat "$MODDIR/latency-provider.sha256")" ] || fail
   "$BB" timeout 2 "$MODDIR/latency-provider" --health --module-dir "$MODDIR" >/dev/null 2>&1 || fail
  else profile_latency_normal_ready "$hal" 0 >/dev/null 2>&1 || fail;fi
  process_matches "$hal" "$ticks" || fail
  printf '%s\n' '{"schema":1,"scope":"application_preflight","normal_removal_possible":true}'
  ;;
 normal)
  [ "$#" = 3 ] || fail
  case "$2:$3" in *[!0-9:]*|0:*|*:0|0?*:*|*:0?*) fail;;esac
  [ "${#2}" -le 10 ] && [ "${#3}" -le 10 ] && [ "$2" -le 2147483647 ] && [ "$3" -le 2147483647 ] || fail
  [ "$2" -gt 1 ] && [ "$3" -gt 1 ] && [ "$2" = "$read_hal" ] || fail
  if process_start "$3" >/dev/null 2>&1;then fail;fi
  [ ! -e "$STATE/latency-runtime.active" ] && [ ! -L "$STATE/latency-runtime.active" ] || fail
  [ ! -e "$STATE/latency-runtime.request" ] && [ ! -L "$STATE/latency-runtime.request" ] || fail
  if [ -e "$STATE/latency-provider.pid" ] || [ -e "$STATE/latency-provider.start" ];then
   trusted "$STATE/latency-provider.pid";trusted "$STATE/latency-provider.start"
   ! process_matches "$(cat "$STATE/latency-provider.pid")" "$(cat "$STATE/latency-provider.start")" || fail
  fi
  ticks=$(process_start "$2") || fail
  profile_latency_recovery_preflight && profile_latency_normal_ready "$2" "$3" >/dev/null 2>&1 || fail
  process_matches "$2" "$ticks" || fail
  printf '{"schema":1,"scope":"application_normal","hal_pid":%s,"hal_start":"%s","normal_owner_verified":true,"stock_gsl_verified":true,"old_relay_removed":true}\n' "$2" "$ticks"
  ;;
 *) fail;;
esac
