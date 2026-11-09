#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/config.sh"
. "$MODDIR/lib/runtime.sh"
fail() { echo "shizuka: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo 'root required' >&2;exit 1; }
if [ "$#" = 0 ];then set -- get;fi
policy_alive() {
 [ -f "$STATE/policy.pid" ] || return 1
 pid=$(cat "$STATE/policy.pid")
 saved_process_matches policy "$pid" && [ -r "/proc/$pid/cmdline" ] && "$BB" tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F 'BtConfig' >/dev/null
}
stop_policy() {
 if policy_alive;then kill -TERM "$pid" 2>/dev/null || true;fi
 rm -f "$STATE/policy.pid" "$STATE/policy.start"
}
case "${1:-get}" in
 stop)
  stop_policy;exit 0;;
esac
expected=$(cat "$MODDIR/policy.sha256")
actual=$("$BB" sha256sum "$MODDIR/policy.dex");actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'policy.dex hash mismatch' >&2;exit 1; }
export CLASSPATH="$MODDIR/policy.dex"
run_config() { app_process /system/bin BtConfig "$CONFIG_FILE" "$LEGACY_CONFIG" "$@"; }
case "${1:-get}" in
 controller-features)
  [ "$#" = 1 ] || { echo 'controller-features takes no arguments' >&2;exit 64; }
  require_qhs_backend
  profile_verify_controller_firmware || fail 'controller firmware does not match the backend'
  exec app_process /system/bin ControllerFeatures;;
 watch)
  [ -f "$MODDIR/state/active" ] || { echo 'module not active' >&2;exit 1; }
  require_qhs_backend
  exec app_process /system/bin BtConfig "$CONFIG_FILE" "$LEGACY_CONFIG" watch;;
 start)
  run_config validate >/dev/null
  if [ "$(run_config value classic_qhs_host_aware)" != true ];then
   stop_policy
   exit 0
  fi
  [ -f "$MODDIR/state/active" ] || fail 'module not active'
  require_qhs_backend
  mkdir -p "$MODDIR/state"
  if policy_alive && [ -f "$STATE/policy.start" ];then exit 0;fi
  # A PID-only record may belong to the old preference-replay watcher.
  stop_policy
  "$BB" setsid "$BB" sh "$0" watch 9>&- > "$MODDIR/state/policy.log" 2>&1 < /dev/null &
  launched=$!
  if ! record_process policy "$launched";then kill -TERM "$launched" 2>/dev/null || true;fail 'policy watcher exited before its identity could be recorded';fi;;
 get|value|validate|set|migrate|device)
  exec app_process /system/bin BtConfig "$CONFIG_FILE" "$LEGACY_CONFIG" "$@";;
 *) echo 'usage: config.sh get|validate|set EXTENSION VALUE|migrate|device ADDRESS|active [get|transport|profile|optional-codecs]|controller-features|start|stop' >&2;exit 1;;
esac
