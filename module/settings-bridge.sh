#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Started only by KernelSU Action; the app never needs its own root grant.
set -eu
BB=/data/adb/ksu/bin/busybox
MODDIR=$("$BB" readlink -f "${0%/*}")
STATE="$MODDIR/state"
fail() { echo "shizuka: $*" >&2; exit 1; }
. "$MODDIR/lib/runtime.sh"
[ "$(id -u)" = 0 ] || fail 'root is required to start the settings bridge'
ensure_busybox_shell "$@"
prepare_state
[ ! -L "$STATE/settings-bridge.lock" ] || fail 'unsafe settings bridge lock'
exec 8> "$STATE/settings-bridge.lock"
"$BB" flock 8
record="$STATE/settings-bridge.json"

stop_bridge() {
 [ -e "$record" ] || return 0
 [ -f "$record" ] && [ ! -L "$record" ] && [ "$("$BB" stat -c '%u' "$record")" = 0 ] || fail 'unsafe settings bridge record'
 [ "$("$BB" stat -c '%s' "$record")" -le 4096 ] || fail 'oversized settings bridge record'
 previous_pid=$("$BB" sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p' "$record")
 previous_start=$("$BB" sed -n 's/.*"start":"\([0-9][0-9]*\)".*/\1/p' "$record")
 previous_socket=$("$BB" sed -n 's/.*"socket":"\(shizuka-settings-[a-f0-9-]*\)".*/\1/p' "$record")
 if process_matches "$previous_pid" "$previous_start";then
  commandline=$("$BB" tr '\000' '\n' < "/proc/$previous_pid/cmdline") || fail 'cannot inspect previous bridge'
  printf '%s\n' "$commandline" | "$BB" grep -Fx SettingsBridge >/dev/null || fail 'previous PID is not a settings bridge'
  printf '%s\n' "$commandline" | "$BB" grep -Fx "$MODDIR" >/dev/null || fail 'previous bridge belongs to another module'
  printf '%s\n' "$commandline" | "$BB" grep -Fx "$previous_socket" >/dev/null || fail 'previous bridge socket does not match'
  kill -TERM "$previous_pid" || true
  count=0
  while process_matches "$previous_pid" "$previous_start" && [ "$count" -lt 20 ];do
   sleep 0.1;count=$((count+1))
  done
  if process_matches "$previous_pid" "$previous_start";then kill -KILL "$previous_pid" || true;fi
 fi
 rm -f "$record"
}

case "${1:-}" in
 stop) [ "$#" = 1 ] || fail 'stop takes no arguments';stop_bridge;exit 0;;
 start) [ "$#" = 2 ] || fail 'start requires the companion UID';companion_uid=$2;;
 *) fail 'usage: settings-bridge.sh start UID|stop';;
esac
case "$companion_uid" in ''|*[!0-9]*) fail 'invalid companion UID';;esac
[ "$companion_uid" -ge 10000 ] && [ "$companion_uid" -le 2147483647 ] || fail 'invalid companion UID'
[ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || fail 'module is disabled or removed'
[ -f "$MODDIR/module.prop" ] || fail 'module is unavailable'
expected=$(cat "$MODDIR/policy.sha256")
actual=$(sha "$MODDIR/policy.dex")
[ "$actual" = "$expected" ] || fail 'policy.dex hash mismatch'
stop_bridge
socket="shizuka-settings-$(cat /proc/sys/kernel/random/uuid)"
export CLASSPATH="$MODDIR/policy.dex"
"$BB" setsid app_process /system/bin SettingsBridge "$MODDIR" "$socket" "$companion_uid" 8>&- > "$STATE/settings-bridge.log" 2>&1 < /dev/null &
attempt=0
while [ "$attempt" -lt 50 ];do
 if [ -f "$record" ] && "$BB" grep -Fq "\"socket\":\"$socket\"" "$record";then
  printf '%s\n' "$socket"
  exit 0
 fi
 sleep 0.1;attempt=$((attempt+1))
done
fail 'settings bridge did not become ready; inspect state/settings-bridge.log'
