#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/config.sh"
. "$MODDIR/lib/runtime.sh"
. "$MODDIR/lib/policy.sh"
fail() { echo "shizuka: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo 'root required' >&2;exit 1; }
if [ "$#" = 0 ];then set -- get;fi
case "${1:-get}" in
 start|stop)
  [ "$#" = 1 ] || fail 'start/stop take no arguments'
  ensure_busybox_shell "$@"
  lock_lifecycle
  lock_policy;;
esac
case "${1:-get}" in
 stop)
  stop_policy;exit 0;;
esac
expected=$(cat "$MODDIR/policy.sha256")
actual=$("$BB" sha256sum "$MODDIR/policy.dex");actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'policy.dex hash mismatch' >&2;exit 1; }
export CLASSPATH="$MODDIR/policy.dex"
case "${1:-get}" in
 controller-features)
  [ "$#" = 1 ] || { echo 'controller-features takes no arguments' >&2;exit 64; }
  require_qhs_backend
  profile_verify_controller_firmware || fail 'controller firmware does not match the backend'
  exec app_process /system/bin ControllerFeatures;;
 watch)
  [ "$#" = 1 ] || fail 'watch takes no arguments'
  [ -f "$MODDIR/state/active" ] || { echo 'module not active' >&2;exit 1; }
  require_qhs_backend
  wait_policy_identity
  exec app_process /system/bin BtConfig "$CONFIG_FILE" "$LEGACY_CONFIG" watch;;
 start)
  start_policy_worker;;
 get|value|validate|set|migrate|device|tuning|help|--help)
  exec app_process /system/bin BtConfig "$CONFIG_FILE" "$LEGACY_CONFIG" "$@";;
 *) echo 'usage: config.sh get|validate|set EXTENSION VALUE|migrate|device ADDRESS|active [get|transport|profile|optional-codecs]|controller-features|start|stop' >&2;exit 1;;
esac
