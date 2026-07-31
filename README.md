# Famicom Expand USB Adapter

ファミリーベーシックキーボードとファミコン光線銃(ザッパー)を、USB HIDキーボード/マウスとしてPCで使えるようにするアダプター。

Arduino Nano(信号読み取り) + Raspberry Pi Pico(CircuitPython, USB HID出力)の2段構成。

![組み上がった実機](hardware/photos/assembled.jpg)

## 全体構成

```
[ファミコン拡張端子(15pin)] --- [Nano: 走査/読み取り] --UART(9600bps)--> [Pico: USB HID出力] --USB--> [PC]
```

- **Nano**: ファミリーベーシックキーボードのマトリクス走査、光線銃(トリガー/センサー)の読み取り。5Vネイティブロジックで駆動(キーボード内部の古いCMOS ICとレベル変換なしで直結するため)。
- **Pico**: CircuitPython動作。UART経由でNanoからのイベントを受信し、USB HIDキーボード/マウスとしてPCに送信。LEDインジケーター制御も担当。

詳しい配線・信号プロトコルは [docs/pinout.md](docs/pinout.md)、配線図は [docs/wiring-diagram.svg](docs/wiring-diagram.svg) を参照してください。

## 実機での動作状況(2026-08-01時点)

- キーボードモード: 動作確認済み
- 光線銃(ザッパー)モード: 実機確認済み(トリガー→左クリック、センサー→右クリック)
- `_` / YENキーのHID Usage値は理論値のまま未実測(要検証)

## ファームウェア

- [`firmware/nano/famicom_scanner.ino`](firmware/nano/famicom_scanner.ino) — Nano側。Arduino IDEで書き込み(ボード: Arduino Nano, ATmega328)。
- [`firmware/pico/code.py`](firmware/pico/code.py) — Pico側。CircuitPython + [adafruit_hid](https://github.com/adafruit/Adafruit_CircuitPython_HID) ライブラリが必要(`CIRCUITPY/lib/adafruit_hid` に配置)。Picoのファームウェア自体もCircuitPythonに書き換えておくこと(Arduino/arduino-picoコアでは不可、経緯は下記参照)。

## ハードウェア

### BOM(部品)

- Arduino Nano x1
- Raspberry Pi Pico x1
- タクトスイッチ x1(モード切替用)
- 抵抗 x6(UART電圧分圧用 10kΩ・22kΩ各1本 + LED電流制限用4本、値は手持ちに合わせて選定)
- LED x4(Shift / Alt / CapsLock / NumLockインジケーター)
- ピンソケット、ピンヘッダー(先曲がり)
- ビニール導線
- ユニバーサル基板: 秋月電子 [103230] 片面ガラスコンポジット・ユニバーサル基板 Bタイプ めっき仕上げ(95×72mm)日本製
- 3Dプリンターで自作した外装(ねじ止め)

外装STLは [`hardware/enclosure/Famicom_ExPand_to_USB_Flame.stl`](hardware/enclosure/Famicom_ExPand_to_USB_Flame.stl)。

### ファミコン拡張端子の取り出し

拡張端子コネクタ単体では基板から取り外せなかったため、ファミコン本体の基板ごと超音波カッターで切り抜いて使用。切り出した基板は結束バンドでユニバーサル基板/外装に固定している。

## 既知の注意点

- キーボードと光線銃はDA15の4番・5番ピンを共用しているため、**同時接続は不可**(実機の前提通り)。トリガーを長押しするとキーボード側のマトリクス走査にも同じ物理ピンの変化が伝わり、擬似的なキーイベントが大量発生することを確認済み(キーボード未接続時は実害なし)。詳細は [docs/pinout.md](docs/pinout.md) 参照。
- Shift/Alt/Ctrlはすべてトグル方式(押すたびにON/OFF反転)。
- Nano→Pico方向のUARTのみ電圧分圧が必要(Pico→Nanoは直結でOK)。

## 開発の経緯・詰まったポイント(教訓)

1. ESP32-S3で検証を開始したが、3.3VロジックとキーボードのCMOS IC(4017/4019/4069)の相性が悪くチャタリング多発。Nanoの5Vネイティブロジックに変更して安定化。
2. キーボード走査プロトコルはnesdev/goroh氏の解析情報がベース。列選択後の待ち時間は寄生容量対策で300us→500usに増やして安定。
3. デバウンスは「行まるごと」ではなく「1キーごと(72キー個別)」でないと機能しなかった。
4. Arduino(arduino-picoコア)のKeyboardライブラリで「キー張り付き」バグに長時間ハマり、原因を特定できないまま**CircuitPython + adafruit_hidへ全面移行**して解決。
5. JIS配列とUS配列のHID Usage ID対応はズレるため、実測して1つずつ補正した。
6. Shift/Alt/Ctrlをトグル式にしたところ、ウォッチドッグ処理が誤ってShiftにも作用し、Windowsの「固定キー機能」を誤爆させた。ウォッチドッグの対象からShift/Alt/Ctrlのトグル状態を除外して解消。
7. Ctrl単体とShift+Alt同時トグルのLED表示が同じになり区別不能だった → 「Ctrl絡みは必ず点滅、Ctrlなしは必ず固定点灯」に統一。
8. モード切替スイッチのGPIOが一時「不調」と判明し別ピンに変更したが、後日の再検証では元のピンで問題なく動作。原因不明のため、また不調が出たら同様に疑うこと。

## 未確認・要検証事項

- `_`(0x87)、YEN(0x89)キーの実測補正
- ザッパーモードのLEDアニメーション速度(`ZAPPER_ANIM_STEP=0.15秒`)の調整

## 開発者より

- 今回の開発では主にClaudeを使っています。(要所要所の精査とか、雑用とかはGeminiやGPTも使ってる)途中で無料版のトークン制限にげんなりして課金しましたが、ClaudeCodeはいいですね、感動した。(周回遅れ)このレポジトリもClaudeがアップしてくれました。
- 検証とかはほぼほぼしてないです。コードが読めないので。基本的に体当たり検証です。
- 多分やろうと思えばPicoだけでもキーボードは使えます。ただ光線銃やその他周辺機器が必ずしもロジック電圧が合うとは限らないので、汎用性を重視してファミコンと同じ5VロジックのNanoをかましています。
- いろいろ質問あれば[https://x.com/R_7_Rocket]

## 参考文献
[https://www.nesdev.org/wiki/Family_BASIC_Keyboard]
[https://nesmd.nomaki.jp/nes2.html]
ファミコンに関するすべてのハッカーに感謝を表します。

