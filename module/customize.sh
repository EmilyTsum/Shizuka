#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
ui_print 'Shizuka Bluetooth / verified device backend'
ui_print 'Exact stock and payload hashes required; late runtime mounts only.'
ui_print 'Codec and device preferences are managed by Android Bluetooth Settings.'
ui_print 'Optional extension settings: /data/adb/shizuka/config.conf'
ui_print 'Legacy extension settings remain readable; device-setting replay is disabled.'
ui_print 'Action opens the native Shizuka settings app.'
ui_print 'Explicit activation: sh /data/adb/modules/s25_aptx_adaptive/apply.sh'
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
[ ! -f "$MODPATH/dsp-capture" ] || set_perm "$MODPATH/dsp-capture" 0 0 0755

# The internal UI is registered by Action from the final module directory.
# Do not install an orphan APK from an installer staging directory.
ui_print 'The Action-only internal UI is included; Action registers it for this Root session.'
