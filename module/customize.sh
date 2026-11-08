#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
ui_print 'Shizuka / SM-S931Z DZIF experimental Bluetooth port'
ui_print 'Exact stock and payload hashes required; late runtime mounts only.'
ui_print 'External preferences: /data/adb/s25_bluetooth/config.conf'
ui_print 'Existing external config is preserved. See config.example.conf.'
ui_print 'Action applies/repairs. It does not disable an active module.'
ui_print 'Rollback: sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
ui_print 'Full reboot flow has not been verified.'
# KernelSU manages WebUI ownership and SELinux labels itself.
for item in "$MODPATH"/*; do
 [ "${item##*/}" = webroot ] && continue
 if [ -d "$item" ]; then
  set_perm_recursive "$item" 0 0 0755 0644
 else
  set_perm "$item" 0 0 0644
 fi
done
