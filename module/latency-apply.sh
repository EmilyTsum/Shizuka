#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Preparation only: packaged Java factory remains unqualified. No audio command is enabled.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
APP_PROCESS=/system/bin/app_process
STATE="$MODDIR/state"
fail() { echo "Shizuka delay application: $*" >&2;exit 1; }
[ -x "$BB" ] || fail 'KernelSU busybox unavailable'
[ "$(id -u)" = 0 ] || fail 'Root is required'
trusted_directory() {
 [ -d "$1" ] && [ ! -L "$1" ] || fail 'Unsafe application directory'
 directory_attributes=$("$BB" stat -c '%u:%a:%h' "$1") || fail 'Cannot inspect application directory'
 case "$directory_attributes" in 0:*) ;;*) fail 'Unsafe application directory owner';;esac
 directory_permissions=${directory_attributes#0:};directory_permissions=${directory_permissions%:*}
 case "$directory_permissions" in ''|*[!0-7]*) fail 'Invalid application directory permissions';;esac
 [ "$((0$directory_permissions & 022))" = 0 ] || fail 'Writable application directory'
}
trusted() {
 [ -f "$1" ] && [ ! -L "$1" ] || fail 'Unsafe packaged application file'
 attributes=$("$BB" stat -c '%u:%a:%h' "$1") || fail 'Cannot inspect packaged application file'
 case "$attributes" in 0:*:1) ;;*) fail 'Unsafe application file owner/link count';;esac
 permissions=${attributes#0:};permissions=${permissions%:1}
 case "$permissions" in ''|*[!0-7]*) fail 'Invalid application file permissions';;esac
 [ "$((0$permissions & 022))" = 0 ] || fail 'Writable packaged application file'
}
trusted_directory "$MODDIR";trusted_directory "$MODDIR/lib"
trusted "$0";trusted "$MODDIR/lib/runtime.sh"
. "$MODDIR/lib/runtime.sh"
ensure_busybox_shell "$@"
[ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || fail 'Module path is not canonical'
[ -d "$MODDIR" ] && [ ! -L "$MODDIR" ] && [ "$("$BB" stat -c '%u' "$MODDIR")" = 0 ] || fail 'Unsafe module directory'
[ ! -e "$MODDIR/disable" ] && [ ! -L "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] && [ ! -L "$MODDIR/remove" ] || fail 'Module unavailable'
DEX="$MODDIR/latency-application.dex"
trusted "$DEX";trusted "$MODDIR/latency-application.sha256"
[ "$("$BB" stat -c '%s' "$DEX")" -le 8388608 ] || fail 'Application DEX exceeds bound'
[ "$("$BB" stat -c '%s' "$MODDIR/latency-application.sha256")" = 65 ] || fail 'Invalid application digest length'
expected=$(cat "$MODDIR/latency-application.sha256")
[ "${#expected}" = 64 ] || fail 'Invalid application digest'
case "$expected" in *[!0-9a-f]*) fail 'Invalid application digest';;esac
[ "$(sha "$DEX")" = "$expected" ] || fail 'Application source artifact mismatch'
prepare_state
java_run() { CLASSPATH="$DEX" "$APP_PROCESS" /system/bin LatencyApplyMain --module-dir "$MODDIR" "$@"; }
lock_bounded() {
 [ ! -L "$STATE/lifecycle.lock" ] || fail 'Unsafe lifecycle lock'
 exec 9> "$STATE/lifecycle.lock"
 "$BB" flock -w "$1" 9 || return 1
}
case "${1:-status}" in
 begin)
  [ "$#" = 4 ] || fail 'begin UUID HQ_MS LL_MS'
  # Mutations use the same BSD flock as normal module lifecycle operations.
  if ! lock_bounded 0;then
   printf '%s\n' '{"schema":1,"scope":"latency_application","accepted":false,"available":false,"applied":false,"phase":"rejected","terminal":true,"reason":"Module lifecycle is busy"}'
   exit 0
  fi
  set +e
  response=$(java_run prepare "$2" "$3" "$4" 9>&-)
  result=$?
  set -e
  case "$result" in
   0) printf '%s\n' "$response";;
   10)
    # No inherited flock or bridge pipe. Android whole-cgroup survival still has
    # to be qualified by the lifecycle adapter before its factory returns true.
    "$BB" setsid "$BB" sh "$0" --worker "$2" </dev/null >/dev/null 2>&1 9>&- &
    printf '%s\n' "$response"
    ;;
   *) exit "$result";;
  esac
  ;;
 status)
  [ "$#" -le 2 ] || fail 'status [UUID]'
  shift;java_run status "$@"
  ;;
 --worker)
  [ "$#" = 2 ] || fail 'Internal worker requires UUID'
  lock_bounded 15 || fail 'Worker could not acquire lifecycle ownership'
  started=$(process_start "$$") || fail 'Cannot identify lifecycle guardian'
  trusted "$MODDIR/latency-apply-guardian.sh";trusted "$MODDIR/latency-apply-executor.sh"
  . "$MODDIR/latency-apply-guardian.sh"
  APPLICATION_OPERATION=$2;APPLICATION_GUARDIAN_START=$started
  application_channel_prepare "$2"
  # The shell keeps FD9; the child cannot accidentally retain its flock. Any
  # death/timeout leaves the journal unresolved for the owner recovery fence.
  "$BB" timeout -k 5 215 "$BB" env "CLASSPATH=$DEX" "$APP_PROCESS" /system/bin LatencyApplyMain \
   --module-dir "$MODDIR" worker "$2" "$$" "$started" 9>&- &
  application_child=$!
  application_child_start=$(process_start "$application_child") || application_child_start=
  if [ -n "$application_child_start" ];then
   child_identity=$("$BB" mktemp "$APPLICATION_CHANNEL/.worker.XXXXXX") || fail 'Cannot stage worker identity'
   chmod 0600 "$child_identity"
   printf '%s %s %s\n' "$application_child" "$application_child_start" "$(cat /proc/sys/kernel/random/boot_id)" > "$child_identity"
   mv -f "$child_identity" "$APPLICATION_CHANNEL/worker.identity"
  fi
  while [ -n "$application_child_start" ] && process_matches "$application_child" "$application_child_start";do
   application_serve_request || true
   sleep 0.025
  done
  set +e;wait "$application_child";application_result=$?;set -e
  exit "$application_result"
  ;;
 *) fail 'Unknown delay application command';;
esac
