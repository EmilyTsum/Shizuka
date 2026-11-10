#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixed guardian destination; this literal source gate cannot be enabled by input.
set -eu
QUALIFIED=0
[ "$QUALIFIED" = 1 ] || exit 10
MODDIR=${0%/*};BB=/data/adb/ksu/bin/busybox
[ "$#" = 11 ] && [ "$(id -u)" = 0 ] && [ -x "$BB" ] || exit 20
[ "$("$BB" readlink -f "$MODDIR")" = "$MODDIR" ] || exit 20
# Sequence belongs only to the private channel, never the lifecycle action ABI.
sequence=${11}
case "$sequence" in [1-9]|[1-9][0-9]|[1-9][0-9][0-9]|[1-9][0-9][0-9][0-9]|10000) ;;*) exit 20;;esac
for source in "$MODDIR/control.sh" "$MODDIR/latency-apply-owned.sh";do
 [ -f "$source" ] && [ ! -L "$source" ] || exit 20
 attributes=$("$BB" stat -c '%u:%a:%h' "$source") || exit 20
 case "$attributes" in 0:*:1) ;;*) exit 20;;esac
 permissions=${attributes#0:};permissions=${permissions%:1}
 case "$permissions" in ''|*[!0-7]*) exit 20;;esac
 [ "$((0$permissions & 022))" = 0 ] || exit 20
done
# The fixed owned handler authenticates all ten fields, package/config/capture,
# guardian ancestry and the inherited FD9. Never call ordinary activate/rollback
# here: it would try to acquire the guardian's existing lifecycle lock again.
exec "$BB" sh "$MODDIR/control.sh" latency-owned "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}"
