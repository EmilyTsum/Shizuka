#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
ui_print 'Shizuka / SM-S931Z DZIF向けBluetooth試験版'
ui_print '原本・生成物のハッシュを照合し、実行時のマウントだけを行います。'
ui_print '設定ファイル：/data/adb/s25_bluetooth/config.conf'
ui_print '既存の設定は保持します。設定例はconfig.example.confを参照してください。'
ui_print '「アクション」で適用・修復します。適用中のモジュールは無効化しません。'
ui_print '元に戻す：sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
ui_print '端末のフル再起動後の動作は未検証です。'
set_perm_recursive "$MODPATH" 0 0 0755 0644
