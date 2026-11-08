#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Opt-in, exact DZIF Hamilton NVM two-bit overlay. Never modifies vendor storage.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
D="$MODDIR/state/qhs-nvm"
T=/vendor/firmware/hmtnv20.b39
F=/vendor/firmware/hmtbtfw20.tlv
ORIG=16c68fb73417b926ad40c2a42a7e3ff9be78db45aa81fddb31fcb069991fb0a6
TEST=4aee484bfa15d22918053caaa13241f3a39463493de26b28779b70e84c7d4028
FW=ff39d6a312648e64f81c933f324c2858c6bb1ee93a71d5440604e2230379fb37
bad() { echo "qhs-nvm: $*" >&2; exit 1; }
sha() { "$BB" sha256sum "$1" | "$BB" awk '{print $1}'; }
secure_dir() {
 [ ! -L "$1" ] && [ -d "$1" ] || bad 'unsafe state directory'
 [ "$("$BB" stat -c '%u:%a' "$1")" = 0:700 ] || bad 'state must be root-owned0700'
}
mount_id() { "$BB" awk -v t="$T" '$5==t {print $1}' /proc/self/mountinfo; }
identity() { "$BB" stat -c '%d:%i' "$1"; }
owned() {
 [ -f "$D/intent" ] && [ ! -L "$D/candidate" ] && [ -f "$D/candidate" ] || return 1
 [ "$(sha "$D/candidate")" = "$TEST" ] && [ "$(sha "$T")" = "$TEST" ] || return 1
 [ "$(identity "$T")" = "$(identity "$D/candidate")" ] || return 1
 if [ -f "$D/mount.id" ];then [ "$(mount_id)" = "$(cat "$D/mount.id")" ] || return 1;fi
 [ "$(cat "$D/boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]
}
# Sourceable function definitions for offline tests end here; no test override
# or alternative filesystem root is accepted by the installed entrypoint.
[ "$(id -u)" = 0 ] || bad 'root required'
[ -x "$BB" ] || bad 'busybox missing'
# Android mksh can mark numeric shell FDs close-on-exec; run every flock
# under the fixed KernelSU ash binary. Inspect executable identity, not an
# inherited environment flag, so ordinary `sh control.sh` is safe too.
bb_exe=$("$BB" readlink -f "$BB")
shell_exe=$("$BB" readlink -f "/proc/$$/exe")
[ -n "$bb_exe" ] && [ -n "$shell_exe" ] || { echo 'Cannot identify shell executable' >&2;exit 1; }
if [ "$shell_exe" != "$bb_exe" ];then
 exec "$BB" sh "$0" "$@"
fi
if [ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ];then
 exec "$BB" nsenter -t 1 -m -- "$BB" sh "$0" "$@"
fi
[ -d "$MODDIR/state" ] || { umask 077; mkdir "$MODDIR/state"; }
secure_dir "$MODDIR/state"
[ -d "$D" ] || { umask 077; mkdir "$D"; }
secure_dir "$D"
[ ! -L "$D/lock" ] || bad 'unsafe lock'
exec 8> "$D/lock"
"$BB" flock 8
[ "$(sha "$F")" = "$FW" ] || bad 'unsupported controller firmware'
case "${1:-}" in
 apply)
  [ "$(getprop persist.vendor.qcom.bluetooth.soc)" = hamilton ] || bad 'unsupported SoC'
  p=$(getprop init.svc_debug_pid.vendor.bluetooth-aidl-qti)
  case "$p" in ''|*[!0-9]*)bad 'controller HAL unavailable';;esac
  [ "$(readlink /proc/$p/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ] || bad 'controller HAL namespace differs'
  mid=$(mount_id)
  if [ -n "$mid" ];then owned || bad 'foreign firmware mount';exit 0;fi
  [ "$(sha "$T")" = "$ORIG" ] || bad 'unsupported NVM'
  # No mount survives boot. Drop stale ownership only after proving stock input.
  if [ -f "$D/boot" ] && [ "$(cat "$D/boot")" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
   rm -f "$D/intent" "$D/mount.id" "$D/scan" "$D/restoring"
  fi
  [ ! -f "$D/restoring" ] || bad 'unfinished runtime restoration'
  for f in candidate boot intent mount.id scan;do [ ! -L "$D/$f" ] || bad 'unsafe state file';done
  tmp=$("$BB" mktemp -d "$D/.prepare.XXXXXX")
  trap 'rm -rf "$tmp"' EXIT
  trap 'exit 1' HUP INT TERM
  "$BB" cp "$T" "$tmp/candidate"
  printf '\003' | "$BB" dd of="$tmp/candidate" bs=1 seek=3817 conv=notrunc 2>/dev/null
  printf '\001' | "$BB" dd of="$tmp/candidate" bs=1 seek=3953 conv=notrunc 2>/dev/null
  [ "$(sha "$tmp/candidate")" = "$TEST" ] || bad 'candidate mismatch'
  chmod 0444 "$tmp/candidate"; chcon u:object_r:vendor_firmware_file:s0 "$tmp/candidate"
  mv "$tmp/candidate" "$D/candidate"
  cat /proc/sys/kernel/random/boot_id > "$tmp/boot"
  mv "$tmp/boot" "$D/boot"
  scan=$(settings get global ble_scan_always_enabled)
  case "$scan" in 0|1);;*)bad 'unsupported BLE scan setting';;esac
  printf '%s\n' "$scan" > "$tmp/scan"
  mv "$tmp/scan" "$D/scan"
  rm -f "$D/mount.id"
  : > "$D/intent"
  "$BB" mount -o bind "$D/candidate" "$T"
  owned || bad 'mounted candidate identity mismatch'
  mount_id > "$tmp/mount.id"
  mv "$tmp/mount.id" "$D/mount.id"
  # Input is already mounted before the scan edge can reopen the controller.
  settings put global ble_scan_always_enabled 0
  ;;
 scan-restore)
  if [ -f "$D/scan" ];then
   v=$(cat "$D/scan");case "$v" in 0|1);;*)bad 'invalid saved scan setting';;esac
   settings put global ble_scan_always_enabled "$v"
   [ "$(settings get global ble_scan_always_enabled)" = "$v" ] || bad 'scan restore failed'
  fi
  ;;
 remove)
  mid=$(mount_id)
  if [ -n "$mid" ];then owned || bad 'foreign firmware mount';"$BB" umount "$T";fi
  [ "$(sha "$T")" = "$ORIG" ] || bad 'original NVM not restored'
  [ -z "$(mount_id)" ] || bad 'firmware mount remains'
  if [ -f "$D/boot" ] && [ "$(cat "$D/boot")" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
   # A previous boot's mount cannot survive. Do not restore an old scan
   # preference over the new boot's current user setting.
   rm -f "$D/intent" "$D/mount.id" "$D/scan" "$D/restoring"
   exit 0
  fi
  if [ -f "$D/intent" ] || [ -f "$D/restoring" ];then
   : > "$D/restoring"
   rm -f "$D/intent" "$D/mount.id"
   # Deliberate scan edge after original file is restored, before radio reset.
   settings put global ble_scan_always_enabled 1
   settings put global ble_scan_always_enabled 0
  fi
  ;;
 finish-remove)
  [ "$(sha "$T")" = "$ORIG" ] && [ -z "$(mount_id)" ] || bad 'original file mismatch'
  rm -f "$D/restoring" "$D/scan"
  ;;
 pending) [ -f "$D/intent" ] || [ -f "$D/restoring" ] ;;
 *) bad 'usage: qhs-nvm.sh apply|remove|scan-restore|finish-remove|pending';;
esac
