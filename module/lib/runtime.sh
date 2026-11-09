# SPDX-License-Identifier: GPL-2.0-only
# Shared by the installed lifecycle scripts; callers set MODDIR, STATE and BB.
ensure_busybox_shell() {
 # mksh may close numeric FDs on exec; KernelSU ash must own every flock.
 [ -x "$BB" ] || fail 'KernelSU busybox unavailable'
 bb_exe=$("$BB" readlink -f "$BB")
 shell_exe=$("$BB" readlink -f "/proc/$$/exe")
 [ -n "$bb_exe" ] && [ -n "$shell_exe" ] || fail 'cannot identify shell executable'
 if [ "$shell_exe" != "$bb_exe" ];then exec "$BB" sh "$0" "$@";fi
}
sha() { value=$("$BB" sha256sum "$1"); echo "${value%% *}"; }
nsrun() { target_pid=$1; shift; "$BB" nsenter -t "$target_pid" -m -- "$@"; }
host_zygote() { /system/bin/ps -A -o PID,PPID,NAME | "$BB" awk '$2==1 && $3=="zygote64" {print $1}'; }
host_bt() {
 host_parent=$(host_zygote)
 /system/bin/ps -A -o PID,PPID,NAME | "$BB" awk -v z="$host_parent" '$2==z && $3=="com.android.bluetooth" {print $1}'
}

# Field 22 follows the parenthesized comm field, which can contain spaces or ).
# A PID alone can refer to a different process after Android reuses it.
process_start() (
 case "$1" in ''|*[!0-9]*|0) return 1;; esac
 process_stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
 process_fields=${process_stat##*) }
 set -f
 # Split stat fields after removing comm; glob expansion is disabled above.
 # shellcheck disable=SC2086
 set -- $process_fields
 [ "$#" -ge 20 ] || return 1
 case "$1" in Z|X) return 1;; esac
 shift 19
 case "$1" in ''|*[!0-9]*) return 1;; esac
 printf '%s\n' "$1"
)
process_matches() { [ -n "$2" ] && [ "$(process_start "$1")" = "$2" ]; }
record_process() (
 started=$(process_start "$2") || return 1
 printf '%s\n' "$started" > "$STATE/$1.start" || return 1
 printf '%s\n' "$2" > "$STATE/$1.pid"
)
saved_process_matches() (
 [ -f "$STATE/$1.pid" ] && [ "$(cat "$STATE/$1.pid")" = "$2" ] || return 1
 if [ -f "$STATE/$1.start" ]; then
  process_matches "$2" "$(cat "$STATE/$1.start")"
 else
  # An activation from an older module has only the PID record.
  kill -0 "$2" 2>/dev/null
 fi
)
token_matches() { [ -n "$2" ] && [ -f "$1" ] && [ "$(cat "$1" 2>/dev/null)" = "$2" ]; }
qhs_pending() { [ -e "$STATE/qhs-nvm/intent" ] || [ -e "$STATE/qhs-nvm/restoring" ]; }
recovery_pending() {
 [ -f "$STATE/active" ] || [ -f "$STATE/armed" ] || [ -s "$STATE/mounts.tsv" ] || qhs_pending
}

prepare_state() {
 [ ! -L "$STATE" ] || fail 'unsafe state symlink'
 if [ -d "$STATE" ]; then
  [ "$("$BB" stat -c '%u' "$STATE")" = 0 ] || fail 'unsafe state owner'
 fi
 (umask 077; mkdir -p "$STATE") || fail 'cannot create state directory'
 chmod 0700 "$STATE" || fail 'cannot protect state directory'
}
lock_lifecycle() {
 prepare_state
 [ ! -L "$STATE/lifecycle.lock" ] || fail 'unsafe lifecycle lock'
 exec 9> "$STATE/lifecycle.lock"
 "$BB" flock 9 || fail 'cannot lock module lifecycle'
 # Detached workers must close FD 9 so they cannot retain this lock.
}

verify_live_file() {
 live_hash=$(nsrun "$1" "$BB" sha256sum "$2"); live_hash=${live_hash%% *}
 [ "$live_hash" = "$(sha "$MODDIR/payload/$3")" ] || fail "NOT APPLIED: $2"
}
# Publishes live_bt/live_hal for the status message after all checks succeed.
verify_active_payloads() {
 check_payloads
 live_bt=$(host_bt || true)
 [ -n "$live_bt" ] && saved_process_matches bt "$live_bt" || fail 'STALE: Bluetooth process changed; use Action to repair'
 live_hal=$(getprop "init.svc_debug_pid.$AUDIO_HAL_SERVICE")
 [ -n "$live_hal" ] && saved_process_matches hal "$live_hal" || fail 'STALE: audio HAL process changed; use Action to repair'
 verify_live_file "$live_bt" "$NATIVE" "$NATIVE_PAYLOAD"
 verify_live_file "$live_bt" "$APK" "$APK_PAYLOAD"
 verify_live_file "$live_hal" "$HAL" "$HAL_PAYLOAD"
}
