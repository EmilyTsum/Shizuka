# SPDX-License-Identifier: GPL-2.0-only
# Lock order: lifecycle FD 9, then policy FD 6. Detached workers close both.
lock_policy() {
 prepare_state
 [ ! -L "$STATE/policy.lock" ] || fail 'unsafe policy lock'
 exec 6> "$STATE/policy.lock"
 "$BB" flock 6 || fail 'cannot lock policy lifecycle'
}
policy_command_matches() (
 pid=$1
 case "$pid" in ''|*[!0-9]*|0) return 1;;esac
 [ -r "/proc/$pid/cmdline" ] || return 1
 command_line=$("$BB" tr '\000' ' ' < "/proc/$pid/cmdline") || return 1
 case "$command_line" in
  *"BtConfig $CONFIG_FILE $LEGACY_CONFIG watch "*|*"BtConfig $LEGACY_CONFIG watch "*) return 0;;
  *) return 1;;
 esac
)
policy_alive() {
 [ -f "$STATE/policy.pid" ] || return 1
 pid=$(cat "$STATE/policy.pid")
 saved_process_matches policy "$pid" && policy_command_matches "$pid"
}
policy_stop_process() (
 watched=$1; started=$2
 process_matches "$watched" "$started" || return 0
 kill -TERM "$watched" 2>/dev/null || { process_matches "$watched" "$started" && return 1;return 0; }
 attempt=0
 while process_matches "$watched" "$started" && [ "$attempt" -lt 20 ];do
  "$BB" sleep 0.1;attempt=$((attempt+1))
 done
 if process_matches "$watched" "$started";then
  kill -KILL "$watched" 2>/dev/null || return 1
  attempt=0
  while process_matches "$watched" "$started" && [ "$attempt" -lt 20 ];do
   "$BB" sleep 0.1;attempt=$((attempt+1))
  done
 fi
 ! process_matches "$watched" "$started"
)
stop_policy() {
 pid=''
 if policy_alive;then
  if [ -f "$STATE/policy.start" ];then started=$(cat "$STATE/policy.start");else started=$(process_start "$pid") || fail 'cannot identify old policy watcher';fi
  policy_stop_process "$pid" "$started" || fail 'policy watcher did not stop; identity records retained'
 elif [ -n "$pid" ] && policy_command_matches "$pid";then
  fail 'policy watcher identity differs; identity records retained'
 fi
 rm -f "$STATE/policy.pid" "$STATE/policy.start"
}
policy_ready() { [ -f "$STATE/policy.log" ] && "$BB" grep -Fqx 'extension_watch=ready; device_preferences=android' "$STATE/policy.log"; }
wait_policy_identity() {
 attempt=0
 while [ "$attempt" -lt 20 ];do
  if saved_process_matches policy "$$" && [ -f "$STATE/policy.start" ];then return 0;fi
  "$BB" sleep 0.1;attempt=$((attempt+1))
 done
 fail 'watcher identity missing; use config.sh start'
}
start_policy_worker() {
 requested=$(extension_value classic_qhs_host_aware) || return 1
 case "$requested" in false) stop_policy;return $?;;true) ;;*) fail 'invalid QHS setting';;esac
 [ -f "$STATE/active" ] || fail 'module not active'
 require_qhs_backend || return 1
 if policy_alive && [ -f "$STATE/policy.start" ] && policy_ready;then return 0;fi
 # Replace PID-only replay watchers and incomplete starts before launching another worker.
 stop_policy || return 1
 : > "$STATE/policy.log" || return 1
 "$BB" setsid "$BB" sh "$MODDIR/config.sh" watch 6>&- 9>&- > "$STATE/policy.log" 2>&1 < /dev/null &
 launched=$!
 launched_start=$(process_start "$launched") || fail 'policy watcher exited before startup'
 if ! record_process policy "$launched";then
  policy_stop_process "$launched" "$launched_start" || true
  fail 'cannot record policy watcher identity'
 fi
 attempt=0
 while process_matches "$launched" "$launched_start" && [ "$attempt" -lt 50 ];do
  if policy_alive && policy_ready;then return 0;fi
  "$BB" sleep 0.1;attempt=$((attempt+1))
 done
 policy_stop_process "$launched" "$launched_start" || fail 'failed watcher did not stop; identity records retained'
 rm -f "$STATE/policy.pid" "$STATE/policy.start"
 fail 'policy watcher failed to become ready; see state/policy.log'
}
