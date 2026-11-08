# Shizuka

Galaxy S25向けの実験的なBluetooth拡張。aptX Adaptive／LLの配線、独立したLossless選択肢、LE Audioの接続・表示修正を含みます。

**対象はSM-S931Z / S931ZSCU1DZIFのみ。** 入力ファイルのハッシュが一致しない端末では使えません。44.1kHzの周期的な音切れは未解決です。Losslessの選択表示は、実際のロスレス伝送成立を保証しません。

## ビルド

メーカー製のAPK・ライブラリは同梱していません。`profile.json` に記載した5ファイルの未改変の原本を `inputs/` に用意してください。

必要なもの：Python 3、ホスト用clang++、JDK 17以降、Android SDK（platform 35／build-tools 35.0.0）、NDK 26.1.10909125、apktool 3.0.3。

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
# JAVA_HOME、ANDROID_SDK_ROOT、S25_NDK_BINを設定
# S25_NDK_BINはNDKのtoolchains/llvm/prebuilt/<host>/bin
python tools/build_all.py
```

生成物は `dist/Shizuka-DZIF-0.6.0-test.zip`。KernelSUでインストールし、モジュールのActionから適用します。新しいビルドには音声確認済みの証明を付けず、15分で自動復元する試験として動作します。手動復元：

```sh
su -c 'sh /data/adb/modules/s25_aptx_adaptive/control.sh rollback'
```

ホストテストのみ：`python tools/verify_offline.py --host-only`（`JAVA_HOME` が必要）。動作状況は [STATUS.md](docs/STATUS.md)。

GPL-2.0-only。ライセンスは本リポジトリのコードと変更部分に適用され、手元のメーカー製ファイルは含みません。
