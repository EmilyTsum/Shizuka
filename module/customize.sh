#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
ui_print 'Shizuka Bluetooth / verified device backend'
ui_print 'Exact stock and payload hashes required; late runtime mounts only.'
ui_print 'Codec and device preferences are managed by Android Bluetooth Settings.'
ui_print 'Optional extension settings: /data/adb/shizuka/config.conf'
ui_print 'Legacy extension settings remain readable; device-setting replay is disabled.'
ui_print 'Open Shizuka from its app icon; KernelSU Action is also available.'
ui_print 'Explicit activation: sh /data/adb/modules/s25_aptx_adaptive/apply.sh'
ui_print 'Rollback: sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
ui_print 'Full reboot flow has not been verified.'
for item in "$MODPATH"/*; do
 [ "$item" != "$MODPATH/webroot" ] || continue
 if [ -d "$item" ]; then
  set_perm_recursive "$item" 0 0 0755 0644
 else
  set_perm "$item" 0 0 0644
 fi
done
[ ! -f "$MODPATH/dsp-capture" ] || set_perm "$MODPATH/dsp-capture" 0 0 0755
[ ! -f "$MODPATH/latency-provider" ] || set_perm "$MODPATH/latency-provider" 0 0 0755

# The internal UI is registered from the final module directory.
# Do not install an orphan APK from an installer staging directory.
ui_print 'The module-owned app is registered automatically from the final module directory.'
ui_print 'Its first independent launch may request KernelSU Root access.'
