#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Temporary-root owned GSL overlay and typed configuration publisher.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
STATE="$MODDIR/state"
. "$MODDIR/lib/runtime.sh"
fail() { echo "Shizuka latency: $*" >&2;exit 1; }
ensure_busybox_shell "$@"
[ "$(id -u)" = 0 ] || fail 'Root is required'
TARGET=/vendor/lib64/libar-gsl.so
STOCK=4d7219f3d1e167c29548aebac62d827c17ef88e5f46be4831d9dd1c003caf6bc
verify_dependencies() {
 for entry in \
  /vendor/lib64/libar-gpr.so:1339b2bcda1415535ec53666ceb4f873c3a7d33faf2a3cb55af3702a0a306d15 \
  /vendor/lib64/liblx-osal.so:1d81e09e7eb9b9442218127036acb255f56857cadd220ed85dbed532619586ef;do
  dependency=${entry%:*};expected=${entry##*:}
  actual=$(nsrun 1 "$BB" sha256sum "$dependency");actual=${actual%% *}
  [ "$actual" = "$expected" ] || fail "Unreviewed runtime dependency: $dependency"
 done
}
REQUEST="$STATE/latency-runtime.request"
ACTIVE="$STATE/latency-runtime.active"
CANDIDATE="$MODDIR/payload/patched-libar-gsl.so"
IDENTITY="$MODDIR/latency-runtime.identity"
REMOTE="$STATE/latency-remote-uncertain"
trusted_file() {
 [ -f "$1" ] && [ ! -L "$1" ] || fail "Unsafe packaged file: $1"
 attributes=$("$BB" stat -c '%u:%a:%h' "$1") || fail 'Cannot inspect packaged file'
 case "$attributes" in 0:*:1) ;;*) fail "Unsafe packaged file owner or link count: $1";;esac
 permissions=${attributes#0:};permissions=${permissions%:1}
 case "$permissions" in ''|*[!0-7]*) fail 'Invalid packaged file permissions';;esac
 [ "$((0$permissions & 022))" = 0 ] || fail "Writable packaged file: $1"
}
valid_boot() {
 [ "${#1}" = 36 ] || return 1
 case "$1" in ????????-????-????-????-????????????) ;;*) return 1;;esac
 case "$1" in *[!0-9a-f-]*) return 1;;esac
 boot_hex=$(printf '%s' "$1" | "$BB" tr -d '-')
 [ "${#boot_hex}" = 32 ]
}
read_packaged_identity() {
 trusted_file "$IDENTITY"
 case "$("$BB" stat -c '%u:%a:%h' "$IDENTITY")" in 0:444:1|0:644:1) ;;*) fail 'Unsafe packaged latency identity permissions';;esac
 [ "$("$BB" stat -c '%s' "$IDENTITY")" = 230 ] || fail 'Invalid packaged latency identity length'
 IFS=' ' read -r identity_version identity_cookie identity_gsl identity_native identity_hal identity_extra < "$IDENTITY" || fail 'Incomplete packaged latency identity'
 [ "$identity_version" = 1 ] && [ -z "${identity_extra:-}" ] || fail 'Unknown packaged latency identity fields'
 [ "${#identity_cookie}" = 32 ] && [ "${#identity_gsl}" = 64 ] && \
  [ "${#identity_native}" = 64 ] && [ "${#identity_hal}" = 64 ] || fail 'Invalid packaged latency digest length'
 [ "$identity_cookie" != 00000000000000000000000000000000 ] || fail 'Empty packaged latency cookie'
 case "$identity_cookie$identity_gsl$identity_native$identity_hal" in *[!0-9a-f]*) fail 'Invalid packaged latency digest';;esac
 [ "$(cat "$IDENTITY")" = "1 $identity_cookie $identity_gsl $identity_native $identity_hal" ] || fail 'Noncanonical packaged latency identity'
 for pair in "$CANDIDATE:$identity_gsl" \
  "$MODDIR/payload/patched-libbluetooth_jni.so:$identity_native" \
  "$MODDIR/payload/patched-libsehbluetooth_audio_session_aidl.so:$identity_hal";do
  packaged=${pair%:*};expected=${pair##*:}
  trusted_file "$packaged"
  [ "$(sha "$packaged")" = "$expected" ] || fail "Packaged runtime/native/HAL pair mismatch: $packaged"
 done
}
read_record() {
 [ -f "$1" ] && [ ! -L "$1" ] && [ "$("$BB" stat -c '%u:%a' "$1")" = 0:600 ] || fail 'Unsafe latency session record'
 IFS=' ' read -r requested_boot cookie candidate_hash mode extra < "$1" || fail 'Incomplete latency session record'
 [ -z "${extra:-}" ] || fail 'Unexpected latency session fields'
 if [ "${2:-}" != allow-stale ];then
  [ "$requested_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] || fail 'Latency request belongs to an earlier boot'
 fi
 [ "${#cookie}" = 32 ] && [ "${#candidate_hash}" = 64 ] || fail 'Invalid latency session identity'
 [ "$cookie" != 00000000000000000000000000000000 ] || fail 'Empty latency activation cookie'
 case "$cookie$candidate_hash" in *[!0-9a-f]*) fail 'Invalid latency session digest';;esac
 case "$mode" in observer|relay) ;;*) fail 'Unknown latency runtime mode';;esac
}
stale_request_destination() {
 valid_boot "$requested_boot" || fail 'Invalid prior request boot identity'
 request_archive="$REQUEST.stale.$requested_boot.$cookie"
 [ ! -e "$request_archive" ] && [ ! -L "$request_archive" ] || fail 'A prior request archive already exists'
}
private_state() {
 [ -d "$STATE" ] && [ ! -L "$STATE" ] && [ "$("$BB" stat -c '%u:%a' "$STATE")" = 0:700 ] || fail 'Unsafe private latency state'
}
process_pair() {
 case "$1:$2" in 0:0) return 0;;esac
 case "$1:$2" in *[!0-9:]*|0:*|*:0|0?*:*|*:0?*) return 1;;esac
 [ -n "$1" ] && [ -n "$2" ] && [ "${#1}" -le 10 ] && [ "${#2}" -le 19 ] && [ "$1" -le 2147483647 ]
}
read_remote() {
 private_state
 trusted_file "$REMOTE"
 [ "$("$BB" stat -c '%u:%a:%h' "$REMOTE")" = 0:600:1 ] || fail 'Unsafe remote recovery journal'
 IFS=' ' read -r remote_version remote_boot remote_cookie remote_hash remote_hal remote_start remote_stopped remote_stopped_start remote_phase remote_reason remote_extra < "$REMOTE" || fail 'Incomplete remote recovery journal'
 [ "$remote_version" = 1 ] && [ -z "${remote_extra:-}" ] && valid_boot "$remote_boot" || fail 'Invalid remote recovery journal'
 if [ "${1:-}" != allow-stale ];then
  [ "$remote_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] || fail 'Remote recovery belongs to an earlier boot; normal owner verification is required'
 fi
 [ "${#remote_cookie}" = 32 ] && [ "${#remote_hash}" = 64 ] && [ "$remote_cookie" != 00000000000000000000000000000000 ] || fail 'Invalid remote recovery identity'
 case "$remote_cookie$remote_hash" in *[!0-9a-f]*) fail 'Invalid remote recovery digest';;esac
 process_pair "$remote_hal" "$remote_start" && process_pair "$remote_stopped" "$remote_stopped_start" || fail 'Invalid remote recovery process identity'
 case "$remote_phase" in clean|pending|ssr_requested|ssr_offline|ssr_ready) ;;*) fail 'Invalid remote recovery phase';;esac
 case "$remote_reason" in status_unknown|remote_ownership) ;;*) fail 'Invalid remote recovery reason';;esac
 remote_text="1 $remote_boot $remote_cookie $remote_hash $remote_hal $remote_start $remote_stopped $remote_stopped_start $remote_phase $remote_reason"
 [ "$(cat "$REMOTE")" = "$remote_text" ] && [ "$("$BB" stat -c '%s' "$REMOTE")" = "$((${#remote_text}+1))" ] || fail 'Noncanonical remote recovery journal'
}
write_remote() {
 private_state
 [ ! -L "$REMOTE" ] || fail 'Unsafe remote recovery journal symlink'
 remote_tmp=$("$BB" mktemp "$STATE/.latency-remote.XXXXXX") || fail 'Cannot stage remote recovery journal'
 chmod 0600 "$remote_tmp";chown 0:0 "$remote_tmp"
 if ! printf '1 %s %s %s %s %s %s %s %s %s\n' "$remote_boot" "$remote_cookie" "$remote_hash" "$remote_hal" "$remote_start" "$remote_stopped" "$remote_stopped_start" "$remote_phase" "$remote_reason" > "$remote_tmp";then
  rm -f "$remote_tmp";fail 'Cannot write remote recovery journal'
 fi
 mv "$remote_tmp" "$REMOTE" || fail 'Cannot publish remote recovery journal'
 "$BB" sync || fail 'Cannot persist remote recovery journal'
}
provider_verified() {
 [ -x "$MODDIR/latency-provider" ] && [ ! -L "$MODDIR/latency-provider" ] && \
  [ -f "$MODDIR/latency-provider.sha256" ] && [ ! -L "$MODDIR/latency-provider.sha256" ] && \
  [ "$(sha "$MODDIR/latency-provider")" = "$(cat "$MODDIR/latency-provider.sha256")" ] && \
  (trusted_file "$MODDIR/latency-provider";trusted_file "$MODDIR/latency-provider.sha256")
}
remote_clean() {
 # The read-only checker binds the raw CRC record to the still-living original
 # HAL PID/start, boot and cookie. A lease may already have expired or vanished.
 [ "$remote_hal" != 0 ] && provider_verified || return 1
 clean_result=$("$BB" timeout 1 "$MODDIR/latency-provider" --recovery-check --module-dir "$MODDIR" \
  --cookie "$remote_cookie" --hal-pid "$remote_hal" --hal-start "$remote_start") || return 1
 printf '%s\n' "$clean_result" | "$BB" grep -q '"safe_without_dsp_restart":true'
}
mark_remote() {
 remote_prior_boot=0
 if [ -e "$REMOTE" ] || [ -L "$REMOTE" ];then
  read_remote allow-stale
  [ "$remote_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] || remote_prior_boot=1
 fi
 [ -e "$ACTIVE" ] || [ -L "$ACTIVE" ] || return 0
 read_record "$ACTIVE" allow-stale
 valid_boot "$requested_boot" || fail 'Invalid activation boot identity'
 if [ -e "$REMOTE" ] || [ -L "$REMOTE" ];then
  [ "$remote_boot" = "$requested_boot" ] && [ "$remote_cookie" = "$cookie" ] && [ "$remote_hash" = "$candidate_hash" ] || fail 'Another remote recovery owns this activation'
  return 0
 fi
 if [ "$requested_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then remote_prior_boot=1;return 0;fi
 [ "$mode" = relay ] || return 0
 remote_boot=$requested_boot;remote_cookie=$cookie;remote_hash=$candidate_hash
 remote_hal=0;remote_start=0;remote_stopped=0;remote_stopped_start=0
 if [ -f "$STATE/hal.pid" ] && [ ! -L "$STATE/hal.pid" ] && [ -f "$STATE/hal.start" ] && [ ! -L "$STATE/hal.start" ];then
  remote_hal=$(cat "$STATE/hal.pid");remote_start=$(cat "$STATE/hal.start")
  process_pair "$remote_hal" "$remote_start" || fail 'Invalid saved HAL identity'
 fi
 remote_phase=pending;remote_reason=remote_ownership
 write_remote
}
recovery_status() {
 shown_phase=none
 if [ -e "$REMOTE" ] || [ -L "$REMOTE" ];then
  shown_phase=$( (read_remote allow-stale;[ "$remote_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] && printf '%s' "$remote_phase" || printf previous_boot) 2>/dev/null) || shown_phase=invalid
 elif [ -e "$ACTIVE" ] || [ -L "$ACTIVE" ];then
  shown_boot=$( (read_record "$ACTIVE" allow-stale;valid_boot "$requested_boot";printf '%s' "$requested_boot") 2>/dev/null) || shown_boot=invalid
  if [ "$shown_boot" = invalid ];then shown_phase=invalid
  elif [ "$shown_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then shown_phase=previous_boot;fi
 fi
 if [ "$shown_phase" != none ];then
  printf '{"schema":1,"scope":"additional_ttp","available":false,"fresh":false,"applied":false,"owned_recovery_required":true,"status":"recovery_required","recovery_phase":"%s","reason":"Remote DSP ownership is unresolved. Complete normal owner recovery before another relay session."}\n' "$shown_phase"
 else
  printf '%s\n' '{"schema":1,"scope":"additional_ttp","owned_recovery_required":false,"recovery_phase":"none"}'
 fi
}
recover_dsp() {
 if [ -e "$REMOTE" ] || [ -L "$REMOTE" ];then
  read_remote allow-stale
 elif [ -e "$ACTIVE" ] || [ -L "$ACTIVE" ];then
  read_record "$ACTIVE" allow-stale
  valid_boot "$requested_boot" || fail 'Invalid activation boot identity'
  [ "$requested_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ] || return 0
  remote_boot=$requested_boot
 else return 0;fi
 recovery_live=$(nsrun 1 "$BB" sha256sum "$TARGET");recovery_live=${recovery_live%% *}
 [ "$recovery_live" = "$STOCK" ] && ! grep -F " $TARGET " /proc/1/mountinfo >/dev/null || fail 'Remove the owned GSL overlay before remote recovery'
 if [ "$remote_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
  valid_boot "$(cat /proc/sys/kernel/random/boot_id)" || fail 'Invalid current boot identity'
  . "$MODDIR/lib/device.sh"
  verify_dependencies
  profile_latency_recovery_preflight || fail 'New-boot stock identity is unconfirmed; ownership records retained'
  echo 'Shizuka latency: a new boot is observed; verify a fresh full normal HAL before retiring old ownership'
  return 0
 fi
 # Only an original, current and still-live HAL can discharge a late CLOSE.
 case "$remote_phase" in clean|pending)
  next_phase=pending;settle=0
  while [ "$settle" -lt 20 ];do
   if remote_clean;then next_phase=clean;break;fi
   sleep 0.1;settle=$((settle+1))
  done
  if [ "$remote_phase" != "$next_phase" ];then remote_phase=$next_phase;write_remote;fi
  ;;
 esac
 [ "$remote_phase" != clean ] || return 0
 . "$MODDIR/lib/device.sh"
 verify_dependencies
 profile_latency_recovery_preflight || fail 'Normal ADSP recovery preflight failed; ownership records retained'
 case "$remote_phase" in
  pending)
   stopped_identity=$(profile_latency_hal_identity) || fail 'Cannot identify the normal audio HAL owner'
   IFS=' ' read -r remote_stopped remote_stopped_start stopped_extra <<EOF
$stopped_identity
EOF
   [ -z "${stopped_extra:-}" ] && process_pair "$remote_stopped" "$remote_stopped_start" || fail 'Invalid normal HAL owner identity'
   write_remote
   profile_latency_stop_hal "$remote_stopped" "$remote_stopped_start" || fail 'Normal audio HAL did not stop; ownership records retained'
   remote_phase=ssr_requested;write_remote
   profile_latency_restart_dsp || fail 'Normal ADSP restart did not complete its request; owner diagnosis is required'
   attempt=0
   while [ "$attempt" -lt 150 ];do
    if [ "$(profile_latency_dsp_state)" = offline ];then remote_phase=ssr_offline;write_remote;break;fi
    sleep 0.1;attempt=$((attempt+1))
   done
   [ "$remote_phase" = ssr_offline ] || fail 'ADSP shutdown was not observed; no automatic repeat is allowed'
   ;;
  ssr_requested) fail 'ADSP restart outcome is unknown; owner diagnosis is required before any repeat';;
  ssr_offline|ssr_ready) ;;
 esac
 if [ "$remote_phase" = ssr_offline ];then
  attempt=0
  while [ "$attempt" -lt 300 ];do
   if [ "$(profile_latency_dsp_state)" = running ];then remote_phase=ssr_ready;write_remote;break;fi
   sleep 0.1;attempt=$((attempt+1))
  done
 fi
 [ "$remote_phase" = ssr_ready ] && [ "$(profile_latency_dsp_state)" = running ] || fail 'Normal ADSP startup is unconfirmed; ownership records retained'
 profile_latency_recovery_preflight || fail 'Post-restart stock identity changed; ownership records retained'
 echo 'Shizuka latency: normal ADSP restart observed; a fresh full audio HAL is still required'
}
archive_old_remote() {
 # A complete new boot invalidates old PID numbers. It is never an SSR request.
 # Validate every archive destination before moving anything; the fence moves last.
 for old_record in "$ACTIVE" "$REQUEST" "$STATE/latency-provider.pid" "$STATE/latency-provider.start" "$REMOTE";do
  if [ -e "$old_record" ] || [ -L "$old_record" ];then
   trusted_file "$old_record"
   old_destination="$old_record.$remote_boot"
   if [ "$old_record" = "$REQUEST" ];then old_destination="$REQUEST.stale.$remote_boot.$remote_cookie";fi
   [ ! -e "$old_destination" ] && [ ! -L "$old_destination" ] || fail 'A prior-boot ownership archive already exists'
  fi
 done
 for old_record in "$ACTIVE" "$REQUEST";do
  if [ -e "$old_record" ];then
   read_record "$old_record" allow-stale
   [ "$requested_boot" = "$remote_boot" ] && [ "$cookie" = "$remote_cookie" ] && [ "$candidate_hash" = "$remote_hash" ] || fail 'A foreign request cannot be retired by old-boot recovery'
  fi
 done
 for old_record in "$ACTIVE" "$REQUEST" "$STATE/latency-provider.pid" "$STATE/latency-provider.start" "$REMOTE";do
  old_destination="$old_record.$remote_boot"
  if [ "$old_record" = "$REQUEST" ];then old_destination="$REQUEST.stale.$remote_boot.$remote_cookie";fi
  if [ -e "$old_record" ];then mv "$old_record" "$old_destination";fi
 done
 "$BB" sync || fail 'Cannot persist prior-boot ownership archive'
}
stop_provider() {
 if [ -f "$STATE/latency-provider.pid" ] && [ -f "$STATE/latency-provider.start" ];then
  pid=$(cat "$STATE/latency-provider.pid");started=$(cat "$STATE/latency-provider.start")
  if process_matches "$pid" "$started" && "$BB" tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F "$MODDIR/latency-provider" >/dev/null;then
   kill -TERM "$pid" 2>/dev/null || true
   count=0
   while process_matches "$pid" "$started" && [ "$count" -lt 20 ];do sleep 0.1;count=$((count+1));done
   if process_matches "$pid" "$started";then
    kill -KILL "$pid" 2>/dev/null || true
    count=0
    while process_matches "$pid" "$started" && [ "$count" -lt 10 ];do sleep 0.1;count=$((count+1));done
   fi
   if process_matches "$pid" "$started";then fail 'Publisher did not exit; ownership records retained';fi
  fi
 fi
 rm -f "$STATE/latency-provider.pid" "$STATE/latency-provider.start"
}
prepare_status() {
 directory=/data/vendor/audio/shizuka
 [ ! -L "$directory" ] || fail 'Unsafe public status directory'
 mkdir -p "$directory";chown root:audio "$directory";chmod 0750 "$directory"
 status="$directory/status.bin"
 [ ! -L "$status" ] || fail 'Unsafe runtime status object'
 if [ -e "$status" ];then
  expected_status="$(id -u audioserver):$(id -g audio):640:1"
  [ -f "$status" ] && [ "$("$BB" stat -c '%u:%g:%a:%h' "$status")" = "$expected_status" ] || fail 'Unexpected runtime status owner/mode/link count'
 else
  (umask 077;set -C; : > "$status") || fail 'Cannot create runtime status object'
 fi
 "$BB" dd if=/dev/zero of="$status" bs=256 count=1 2>/dev/null
 chown audioserver:audio "$status";chmod 0640 "$status"
 restorecon -R "$directory"
}
case "${1:-status}" in
 request|request-owned)
  request_inherited=0
  if [ "$1" = request-owned ];then
   [ "$#" = 11 ] || fail 'owned request requires ten fixed owner arguments'
   request_inherited=1;shift
  else
   [ "$#" = 1 ] || fail 'usage: latency-runtime.sh request'
  fi
  [ -d "$MODDIR" ] && [ ! -L "$MODDIR" ] && [ "$("$BB" stat -c '%u' "$MODDIR")" = 0 ] || fail 'Unsafe module directory'
  [ ! -e "$MODDIR/disable" ] && [ ! -L "$MODDIR/disable" ] && \
   [ ! -e "$MODDIR/remove" ] && [ ! -L "$MODDIR/remove" ] || fail 'Module is unavailable'
  trusted_file "$MODDIR/module.prop";trusted_file "$MODDIR/device-profile"
  [ "$("$BB" sed -n 's/^id=//p' "$MODDIR/module.prop")" = s25_aptx_adaptive ] || fail 'Unexpected module identity'
  [ "$(cat "$MODDIR/device-profile")" = samsung-sm-s931z-dzif ] || fail 'Unreviewed latency device profile'
  [ -d "$MODDIR/payload" ] && [ ! -L "$MODDIR/payload" ] && \
   [ "$("$BB" stat -c '%u' "$MODDIR/payload")" = 0 ] || fail 'Unsafe payload directory'
  if [ "$request_inherited" = 1 ];then
   trusted_file "$MODDIR/latency-apply-owned.sh"
   # The fixed read-only guard verifies the current armed receipt, exact
   # inherited FD9/guardian, package/config/boot and deadline. No env bypass.
   "$BB" sh "$MODDIR/latency-apply-owned.sh" validate-request "$@"
  else
   lock_lifecycle
  fi
  [ -d "$STATE" ] && [ ! -L "$STATE" ] && [ "$("$BB" stat -c '%u:%a' "$STATE")" = 0:700 ] || fail 'Unsafe private latency state'
  [ ! -e "$ACTIVE" ] && [ ! -L "$ACTIVE" ] || fail 'An unresolved latency activation is present'
  [ ! -e "$REMOTE" ] && [ ! -L "$REMOTE" ] || fail 'Remote DSP recovery is required before another request'
  read_packaged_identity
  verify_dependencies
  current_boot=$(cat /proc/sys/kernel/random/boot_id)
  valid_boot "$current_boot" || fail 'Invalid current boot identity'
  stale_request=0
  if [ -e "$REQUEST" ] || [ -L "$REQUEST" ];then
   [ "$("$BB" stat -c '%u:%a:%h' "$REQUEST")" = 0:600:1 ] || fail 'Unsafe latency request owner/mode/link count'
   read_record "$REQUEST" allow-stale
   valid_boot "$requested_boot" || fail 'Invalid saved boot identity'
   [ "$(cat "$REQUEST")" = "$requested_boot $cookie $candidate_hash $mode" ] && \
    [ "$("$BB" stat -c '%s' "$REQUEST")" = "$((36 + 32 + 64 + ${#mode} + 4))" ] || fail 'Noncanonical latency request'
   if [ "$requested_boot" = "$current_boot" ];then
    [ "$cookie" = "$identity_cookie" ] && [ "$candidate_hash" = "$identity_gsl" ] && \
     [ "$mode" = relay ] || fail 'Another latency request already owns this boot'
    echo 'Shizuka latency: current-boot relay request already prepared'
    exit 0
   fi
   stale_request_destination
   stale_request=1
  fi
  temporary=$("$BB" mktemp "$STATE/.latency-runtime.XXXXXX")
  trap '[ -z "${temporary:-}" ] || rm -f "$temporary"' EXIT
  chmod 0600 "$temporary";chown 0:0 "$temporary"
  printf '%s %s %s relay\n' "$current_boot" "$identity_cookie" "$identity_gsl" > "$temporary"
  if [ "$stale_request" = 1 ];then mv "$REQUEST" "$request_archive";fi
  mv "$temporary" "$REQUEST";temporary=''
  echo 'Shizuka latency: current-boot relay request prepared; activation has not started'
  ;;
 mount)
  [ ! -e "$REMOTE" ] && [ ! -L "$REMOTE" ] || fail 'Remote DSP recovery is required before mounting'
  [ -e "$REQUEST" ] || exit 0
  read_record "$REQUEST" allow-stale
  if [ "$requested_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ];then
   stale_request_destination
   mv "$REQUEST" "$request_archive"
   echo 'Shizuka latency: earlier-boot request skipped'
   exit 0
  fi
  [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || fail 'Module is unavailable'
  # Recheck the canonical native/HAL pair when the prepared relay is consumed.
  # A module update after request creation must not mix an older context ABI
  # with a newer GSL runtime. Observer is the existing root-only legacy trial.
  if [ "$mode" = relay ];then
   read_packaged_identity
   [ "$cookie" = "$identity_cookie" ] && [ "$candidate_hash" = "$identity_gsl" ] || fail 'Relay request differs from the packaged runtime identity'
  fi
  [ -f "$CANDIDATE" ] && [ ! -L "$CANDIDATE" ] && [ "$(sha "$CANDIDATE")" = "$candidate_hash" ] || fail 'GSL candidate hash mismatch'
  live=$(nsrun 1 "$BB" sha256sum "$TARGET");live=${live%% *}
  [ "$live" = "$STOCK" ] || fail 'GSL is not the reviewed stock build'
  verify_dependencies
  ! grep -F " $TARGET " /proc/1/mountinfo >/dev/null || fail 'Another GSL overlay is present'
  chown 0:0 "$CANDIDATE";chmod 0444 "$CANDIDATE";chcon u:object_r:vendor_file:s0 "$CANDIDATE"
  [ ! -e "$ACTIVE" ] && [ ! -L "$ACTIVE" ] || fail 'An unresolved latency activation is present'
  temporary=$("$BB" mktemp "$STATE/.latency-runtime.XXXXXX")
  trap '[ -z "${temporary:-}" ] || rm -f "$temporary"' EXIT
  chmod 0600 "$temporary";chown 0:0 "$temporary"
  cat "$REQUEST" > "$temporary"
  mv "$temporary" "$ACTIVE";temporary=''
  touch "$STATE/experiment-unverified"
  prepare_status
  nsid=$("$BB" readlink /proc/1/ns/mnt)
  # Record the owned target before mutation, including the partial-failure case.
  printf '1 %s %s\n' "$nsid" "$TARGET" >> "$STATE/mounts.tsv"
  nsrun 1 "$BB" mount -o bind "$CANDIDATE" "$TARGET"
  live=$(nsrun 1 "$BB" sha256sum "$TARGET");live=${live%% *}
  [ "$live" = "$candidate_hash" ] || fail 'Mounted GSL hash mismatch'
  ;;
 start)
  [ ! -e "$REMOTE" ] && [ ! -L "$REMOTE" ] || fail 'Remote DSP recovery is required before publication'
  [ -e "$ACTIVE" ] || exit 0
  read_record "$ACTIVE"
  [ -f "$STATE/active" ] && [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || fail 'No active module session'
  [ -f "$MODDIR/latency-provider.sha256" ] && [ "$(sha "$MODDIR/latency-provider")" = "$(cat "$MODDIR/latency-provider.sha256")" ] || fail 'Configuration publisher hash mismatch'
  stop_provider
  directory=/data/vendor/audio/shizuka
  hal=$(cat "$STATE/hal.pid")
  "$BB" setsid "$MODDIR/latency-provider" --module-dir "$MODDIR" --output-dir "$directory" --cookie "$cookie" --hal-pid "$hal" 9>&- > "$STATE/latency-provider.log" 2>&1 < /dev/null &
  record_process latency-provider "$!" || fail 'Publisher failed to start'
  sleep 1
  saved_process_matches latency-provider "$(cat "$STATE/latency-provider.pid")" || fail 'Publisher exited during startup'
  ;;
 stop) mark_remote;if [ "$remote_prior_boot" = 0 ];then stop_provider;fi;;
 recovery-status) recovery_status;;
 recover-dsp) [ "$#" = 1 ] || fail 'usage: latency-runtime.sh recover-dsp';recover_dsp;;
 verify)
  [ -e "$ACTIVE" ] || exit 0
  read_record "$ACTIVE"
  [ "$#" = 2 ] || fail 'HAL identity is required'
  live=$(nsrun "$2" "$BB" sha256sum "$TARGET");live=${live%% *}
  [ "$live" = "$candidate_hash" ] || fail 'Running GSL runtime differs from this session'
  grep -Fq 'libar-gsl.so' "/proc/$2/maps" || fail 'GSL runtime is not mapped'
  ;;
 health)
  [ ! -e "$REMOTE" ] && [ ! -L "$REMOTE" ] || fail 'Remote DSP recovery is required'
  [ -e "$ACTIVE" ] || [ -L "$ACTIVE" ] || exit 0
  read_record "$ACTIVE"
  [ -f "$STATE/active" ] && [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || fail 'No active module session'
  [ -f "$MODDIR/latency-provider" ] && [ ! -L "$MODDIR/latency-provider" ] && \
   [ -f "$MODDIR/latency-provider.sha256" ] && [ ! -L "$MODDIR/latency-provider.sha256" ] && \
   [ "$(sha "$MODDIR/latency-provider")" = "$(cat "$MODDIR/latency-provider.sha256")" ] || fail 'Configuration publisher hash mismatch'
  [ -f "$STATE/latency-provider.pid" ] && [ -f "$STATE/latency-provider.start" ] && \
   saved_process_matches latency-provider "$(cat "$STATE/latency-provider.pid")" || fail 'Configuration publisher process ended or changed'
  "$MODDIR/latency-provider" --health --module-dir "$MODDIR"
  ;;
 recovered)
  mark_remote
  if [ "$remote_prior_boot" = 0 ];then stop_provider;fi
  live=$(nsrun 1 "$BB" sha256sum "$TARGET");live=${live%% *}
  [ "$live" = "$STOCK" ] || fail 'Stock GSL was not restored'
  if [ "$remote_prior_boot" = 1 ];then
   [ "$#" = 2 ] || fail 'A fresh full normal HAL is required to retire old-boot ownership'
   if [ -e "$REMOTE" ];then read_remote allow-stale;else
    read_record "$ACTIVE" allow-stale
    remote_boot=$requested_boot;remote_cookie=$cookie;remote_hash=$candidate_hash
   fi
   valid_boot "$(cat /proc/sys/kernel/random/boot_id)" && [ "$remote_boot" != "$(cat /proc/sys/kernel/random/boot_id)" ] || fail 'A different valid boot is required'
   . "$MODDIR/lib/device.sh"
   verify_dependencies
   profile_latency_recovery_preflight && profile_latency_normal_ready "$2" 0 || fail 'New-boot normal audio readiness is blocked; retain old ownership'
   archive_old_remote
   echo 'Shizuka latency: verified new-boot stock DSP and normal audio HAL; prior ownership archived'
   exit 0
  fi
  if [ -e "$REMOTE" ] || [ -L "$REMOTE" ];then
   read_remote
   [ "$#" = 2 ] || fail 'A fresh full HAL is required'
   case "$remote_phase" in clean|ssr_ready) ;;*) fail 'Verified ADSP recovery is required before finalization';;esac
   . "$MODDIR/lib/device.sh"
   previous_normal=$remote_stopped
   [ "$previous_normal" != 0 ] || previous_normal=$remote_hal
   profile_latency_recovery_preflight && profile_latency_normal_ready "$2" "$previous_normal" || fail 'Normal audio readiness is blocked; retain ownership and inspect diagnostics'
  elif [ -e "$ACTIVE" ];then
   read_record "$ACTIVE"
   if [ "$mode" = relay ];then
    [ "$#" = 2 ] || fail 'A fresh full HAL identity is required'
    . "$MODDIR/lib/device.sh"
    old_hal=$(cat "$STATE/hal.pid")
    profile_latency_normal_ready "$2" "$old_hal" || fail 'Normal audio readiness is unconfirmed'
   fi
  fi
  rm -f "$ACTIVE"
  if [ -e "$REMOTE" ];then rm -f "$REMOTE";"$BB" sync;fi
  ;;
 status)
  if [ -e "$ACTIVE" ];then read_record "$ACTIVE";echo "latency_runtime=$mode; boot=$requested_boot";else echo 'latency_runtime=inactive';fi
  ;;
 *) fail 'usage: latency-runtime.sh request|mount|start|stop|verify HAL_PID|health|recovery-status|recover-dsp|recovered [HAL_PID]|status';;
esac
