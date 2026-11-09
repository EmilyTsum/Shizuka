# SPDX-License-Identifier: GPL-2.0-only
RESETPROP=/data/adb/ksu/bin/resetprop

# Snapshot filename, Android property, and the old module's absence semantics.
managed_properties() {
 cat <<'PROPERTIES'
r21 persist.vendor.qcom.bluetooth.aptxadaptiver2_1_support present
dual-mode persist.bluetooth.enable_dual_mode_audio tracked
samsung-cap persist.bluetooth.samsung.a2dp_offload.cap present
snoop persist.bluetooth.btsnooplogmode present
aptx-voice bluetooth.hfp.codec_aptx_voice.enabled tracked
ad-priority bluetooth.a2dp.source.aptx_adaptive_priority.config tracked
hd-priority bluetooth.a2dp.source.aptx_hd_priority.config tracked
PROPERTIES
}
property_list_contains() { "$BB" awk -v key="[$1]:" '$1==key {found=1} END {exit !found}'; }
save_properties() {
 property_list=$(getprop) || return 1
 managed_properties | while read -r saved property legacy; do
  getprop "$property" > "$STATE/$saved" || exit 1
  rm -f "$STATE/$saved-existed" || exit 1
  if printf '%s\n' "$property_list" | property_list_contains "$property"; then
   touch "$STATE/$saved-existed" || exit 1
  fi
 done || return 1
 # Older snapshots did not track absence for all properties.
 : > "$STATE/properties-v1"
}
restore_properties() {
 current_property_list=$(getprop) || return 1
 managed_properties | while read -r saved property legacy; do
  [ -f "$STATE/$saved" ] || continue
  if [ -f "$STATE/$saved-existed" ] || { [ ! -f "$STATE/properties-v1" ] && [ "$legacy" = present ]; }; then
   "$RESETPROP" -n "$property" "$(cat "$STATE/$saved")" || exit 1
  elif printf '%s\n' "$current_property_list" | property_list_contains "$property"; then
   "$RESETPROP" -n -d "$property" || exit 1
  fi
 done
}
