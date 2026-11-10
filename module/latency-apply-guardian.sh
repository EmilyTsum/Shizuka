# SPDX-License-Identifier: GPL-2.0-only
# Sourced only by latency-apply.sh while its shell owns the lifecycle flock FD9.
application_decimal() {
 case "$1" in ''|*[!0-9]*|0?*) return 1;;esac
 [ "${#1}" -le "$2" ]
}
application_uuid() {
 [ "${#1}" = 36 ] || return 1
 case "$1" in ????????-????-????-????-????????????) ;;*) return 1;;esac
 case "$1" in *[!0-9a-f-]*) return 1;;esac
 application_hex=$(printf '%s' "$1" | "$BB" tr -d '-')
 [ "${#application_hex}" = 32 ]
}
application_channel_prepare() {
 application_uuid "$1" || fail 'Invalid guardian operation UUID'
 APPLICATION_CHANNEL="$STATE/latency-application/channel.$1"
 [ ! -L "$STATE/latency-application" ] && [ ! -L "$APPLICATION_CHANNEL" ] || fail 'Unsafe guardian directory'
 (umask 077;mkdir -p "$APPLICATION_CHANNEL") || fail 'Cannot create private guardian channel'
 [ "$("$BB" stat -c '%u:%a' "$STATE/latency-application")" = 0:700 ] && \
  [ "$("$BB" stat -c '%u:%a' "$APPLICATION_CHANNEL")" = 0:700 ] || fail 'Unsafe private guardian channel'
 APPLICATION_SEQUENCE=0
}
application_serve_request() {
 request="$APPLICATION_CHANNEL/request"
 [ -e "$request" ] || [ -L "$request" ] || return 0
 # Malformed input never reaches an executor. Retain it unresolved and let the
 # bounded worker fail; the guardian keeps FD9 until that worker is reaped.
 [ -f "$request" ] && [ ! -L "$request" ] && [ "$("$BB" stat -c '%u:%a:%h' "$request")" = 0:600:1 ] || return 1
 [ "$("$BB" stat -c '%s' "$request")" -le 512 ] || return 1
 IFS=' ' read -r wire sequence action operation hq ll deadline token config flag extra < "$request" || return 1
 [ "$wire" = SHZAPPLY1 ] && [ -z "${extra:-}" ] && [ "$operation" = "$APPLICATION_OPERATION" ] || return 1
 application_uuid "$operation" && application_decimal "$sequence" 5 && [ "$sequence" -le 10000 ] && \
  [ "$sequence" = "$((APPLICATION_SEQUENCE+1))" ] || return 1
 case "$action" in 1|2|3|4|5) ;;*) return 1;;esac
 application_decimal "$hq" 3 && [ "$hq" -le 150 ] && application_decimal "$ll" 3 && [ "$ll" -le 100 ] || return 1
 application_decimal "$deadline" 16 && [ "$deadline" != 0 ] || return 1
 [ "${#token}" = 64 ] && [ "${#config}" = 64 ] || return 1
 case "$token$config" in *[!0-9a-f]*) return 1;;esac
 case "$flag" in 0|1) ;;*) return 1;;esac
 canonical="SHZAPPLY1 $sequence $action $operation $hq $ll $deadline $token $config $flag"
 [ "$(cat "$request")" = "$canonical" ] && [ "$("$BB" stat -c '%s' "$request")" = "$((${#canonical}+1))" ] || return 1
 APPLICATION_SEQUENCE=$sequence
 set +e
 "$BB" sh "$MODDIR/latency-apply-executor.sh" "$action" "$operation" "$hq" "$ll" "$deadline" "$token" "$config" "$flag" \
  "$$" "$APPLICATION_GUARDIAN_START" "$sequence" >/dev/null 2>&1
 executor_result=$?
 set -e
 case "$executor_result" in 0|10|20|30) ;;*) executor_result=20;;esac
 # Remove the consumed request before publication: a fast next request cannot
 # be deleted after the client sees its previous response.
 rm -f "$request"
 reply=$("$BB" mktemp "$APPLICATION_CHANNEL/.response.XXXXXX") || return 1
 (umask 077;printf '%s\n' "SHZAPPLY1 $sequence $action $operation $executor_result" > "$reply") || return 1
 chmod 0600 "$reply";mv -f "$reply" "$APPLICATION_CHANNEL/response"
}
