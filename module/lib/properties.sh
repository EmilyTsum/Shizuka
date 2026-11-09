# SPDX-License-Identifier: GPL-2.0-only
RESETPROP=/data/adb/ksu/bin/resetprop

# Snapshot filename, Android property, and the old module's absence semantics.
managed_properties() { profile_managed_properties; }
property_list_contains() { "$BB" awk -v key="[$1]:" '$1==key {found=1} END {exit !found}'; }
save_properties() {
 property_list=$(getprop) || return 1
 managed_properties | while read -r saved property legacy; do
  getprop "$property" > "$STATE/$saved" || exit 1
  rm -f "$STATE/$saved-existed" "$STATE/$saved-written" || exit 1
  if printf '%s\n' "$property_list" | property_list_contains "$property"; then
   touch "$STATE/$saved-existed" || exit 1
  fi
 done || return 1
 # Older snapshots did not track absence for all properties.
 : > "$STATE/properties-v1"
 : > "$STATE/property-writes"
 : > "$STATE/properties-v2"
}
set_managed_property() (
 saved=$1
 property=$(managed_properties | "$BB" awk -v key="$saved" '$1==key {print $2; found=1} END {exit !found}') || return 1
 [ -f "$STATE/properties-v2" ] && [ -f "$STATE/$saved" ] || return 1
 [ ! -e "$STATE/$saved-written" ] || return 1
 current_list=$(getprop) || return 1
 current=$(getprop "$property") || return 1
 if printf '%s\n' "$current_list" | property_list_contains "$property" && [ "$current" = "$2" ];then return 0;fi
 # Journal the intent before the write so interrupted activation remains recoverable.
 printf '%s\n' "$2" > "$STATE/$saved-written" || return 1
 printf '%s\n' "$saved" >> "$STATE/property-writes" || return 1
 "$RESETPROP" -n "$property" "$2"
)
restore_properties() {
 if [ -f "$STATE/properties-v2" ];then
  [ -f "$STATE/property-writes" ] || return 1
  known=$(managed_properties | "$BB" awk '{print $1}') || return 1
  written=''
  while IFS= read -r written || [ -n "$written" ];do
   [ -n "$written" ] && printf '%s\n' "$known" | "$BB" grep -Fxq -- "$written" || return 1
   [ -f "$STATE/$written-written" ] || return 1
  done < "$STATE/property-writes"
 fi
 current_property_list=$(getprop) || return 1
 managed_properties | while read -r saved property legacy; do
  [ -f "$STATE/$saved" ] || continue
  if [ -f "$STATE/properties-v2" ] && ! "$BB" grep -Fxq "$saved" "$STATE/property-writes";then continue;fi
  if [ -f "$STATE/properties-v2" ];then
   # Preserve settings changed by Android/the user after our write.
   printf '%s\n' "$current_property_list" | property_list_contains "$property" || continue
   current=$(getprop "$property") || exit 1
   [ "$current" = "$(cat "$STATE/$saved-written")" ] || continue
  fi
  if [ -f "$STATE/$saved-existed" ] || { [ ! -f "$STATE/properties-v1" ] && [ "$legacy" = present ]; }; then
   "$RESETPROP" -n "$property" "$(cat "$STATE/$saved")" || exit 1
  elif printf '%s\n' "$current_property_list" | property_list_contains "$property"; then
   "$RESETPROP" -n -d "$property" || exit 1
  fi
 done
}
