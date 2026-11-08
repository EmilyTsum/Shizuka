#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Idempotent apply/repair. Unheard generations get a bounded trial, never a fake audio proof.
set -eu
MODDIR=${0%/*}
BB=/data/adb/ksu/bin/busybox
rm -f "$MODDIR/.no-auto"
if [ ! -f "$MODDIR/proofs/verified-payloads.sha256" ] || ! "$BB" cmp -s "$MODDIR/payloads.sha256" "$MODDIR/proofs/verified-payloads.sha256";then
 if [ -f "$MODDIR/state/active" ];then
  "$BB" sh "$MODDIR/control.sh" status && exit 0
  "$BB" sh "$MODDIR/control.sh" rollback
 fi
 "$BB" sh "$MODDIR/control.sh" preflight
 "$BB" sh "$MODDIR/control.sh" activate 900
 echo 'Audio verification pending: 15-minute trial. Automatic rollback remains armed.'
else
 "$BB" sh "$MODDIR/boot-activate.sh" manual
fi
"$BB" sh "$MODDIR/control.sh" status
