# SPDX-License-Identifier: GPL-2.0-only
# Fixed app-owned paths; configuration is parsed by Java, never sourced by a shell.
CONFIG_DIR=/data/adb/shizuka
CONFIG_FILE="$CONFIG_DIR/config.conf"
LEGACY_CONFIG=/data/adb/s25_bluetooth/config.conf
config_exists() { [ -e "$CONFIG_FILE" ] || [ -L "$CONFIG_FILE" ] || [ -e "$LEGACY_CONFIG" ] || [ -L "$LEGACY_CONFIG" ]; }
extension_value() {
 if config_exists;then
  "$BB" sh "$MODDIR/config.sh" value "$1"
 else
  case "$1" in classic_qhs_host_aware) echo false;;aptx_voice_enabled|adaptive_priority|aptx_hd_priority) echo system;;*) return 1;;esac
 fi
}
require_qhs_backend() {
 [ -f "$MODDIR/device-profile" ] || fail 'controller extensions require a verified device backend'
 . "$MODDIR/lib/device.sh"
 [ "${QHS_SUPPORTED:-false}" = true ] || fail 'controller extension unsupported by this backend'
}
