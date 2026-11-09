# SPDX-License-Identifier: GPL-2.0-only
# Only bundled, reviewed profile files can supply device-specific runtime behavior.
DEVICE_PROFILE=$(cat "$MODDIR/device-profile")
case "$DEVICE_PROFILE" in ''|*[!a-z0-9-]*) fail 'invalid device profile';;esac
[ -f "$MODDIR/profiles/$DEVICE_PROFILE.sh" ] || fail 'unsupported device profile'
# shellcheck source=/dev/null
. "$MODDIR/profiles/$DEVICE_PROFILE.sh"
for hook in profile_target_allowed profile_payload_allowed profile_managed_properties profile_apply_properties profile_hal_ready;do
 command -v "$hook" >/dev/null 2>&1 || fail "device backend is missing $hook"
done
