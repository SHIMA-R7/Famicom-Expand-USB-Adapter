#include <SoftwareSerial.h>

const int OUT0 = 5;
const int OUT1 = 6;
const int OUT2 = 7;
const int D1 = 8;
const int D2 = 9;
const int D3 = 10;
const int D4 = 11;

const int ZAPPER_TRIGGER = 11; // DA15 4番ピン ※D4と物理配線共用(排他利用前提)
const int ZAPPER_SENSOR  = 10; // DA15 5番ピン ※D3と物理配線共用(排他利用前提)

// SFCコントローラーポート(1P/2P)。CLOCK/LATCHは共通、DATAのみポートごと。
const int SFC_CLOCK    = A5;
const int SFC_LATCH    = A0;
const int SFC_DATA_1P  = A1;
const int SFC_DATA_2P  = A3;
// A2(1P D1)・A4(2P D1)は標準パッド/マウスとも未使用のため未配線

SoftwareSerial toPico(2, 3); // RX=D2, TX=D3(分圧経由でPico GP1へ)

bool stableState[9][2][4];
bool candidateState[9][2][4];
uint8_t candidateCount[9][2][4];
bool firstScan = true;
const uint8_t DEBOUNCE_THRESHOLD = 2;

bool lastTrigger = HIGH;
bool lastSensor = HIGH;

// SFC 1P/2Pの状態(id 74〜85=1Pボタン、86〜97=2Pボタン、98=1Pマウス、99=2Pマウス)
const uint8_t SFC1_BUTTON_BASE = 74;
const uint8_t SFC2_BUTTON_BASE = 86;
const uint8_t SFC1_MOUSE_ID = 98;
const uint8_t SFC2_MOUSE_ID = 99;
// ボタン並び(クロック順): B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R
bool sfc1PadButtons[12] = {false};
bool sfc2PadButtons[12] = {false};
bool sfc1IsMouse = false;
bool sfc2IsMouse = false;
uint8_t sfc1MouseButtons = 0;
uint8_t sfc2MouseButtons = 0;

void writeOut(uint8_t v) {
  digitalWrite(OUT0, v & 1);
  digitalWrite(OUT1, (v >> 1) & 1);
  digitalWrite(OUT2, (v >> 2) & 1);
}

uint8_t readNibble() {
  uint8_t b = 0;
  b |= digitalRead(D1) << 0;
  b |= digitalRead(D2) << 1;
  b |= digitalRead(D3) << 2;
  b |= digitalRead(D4) << 3;
  return b;
}

void sendEvent(int row, int col, int bit, bool pressed) {
  uint8_t id = row * 8 + col * 4 + bit;
  uint8_t packet = (pressed ? 0x80 : 0x00) | id;
  toPico.write(packet);
}

void processBit(int row, int col, int bit, bool rawOn) {
  if (firstScan) {
    stableState[row][col][bit] = rawOn;
    candidateState[row][col][bit] = rawOn;
    candidateCount[row][col][bit] = DEBOUNCE_THRESHOLD;
    return;
  }
  if (rawOn == candidateState[row][col][bit]) {
    if (candidateCount[row][col][bit] < 255) candidateCount[row][col][bit]++;
  } else {
    candidateState[row][col][bit] = rawOn;
    candidateCount[row][col][bit] = 1;
  }
  if (candidateCount[row][col][bit] >= DEBOUNCE_THRESHOLD &&
      stableState[row][col][bit] != rawOn) {
    sendEvent(row, col, bit, !rawOn);
    stableState[row][col][bit] = rawOn;
  }
}

void scanKeyboard() {
  writeOut(0b101);
  delayMicroseconds(500);
  for (int row = 0; row < 9; row++) {
    writeOut(0b100);
    delayMicroseconds(500);
    uint8_t col0 = readNibble();
    writeOut(0b110);
    delayMicroseconds(500);
    uint8_t col1 = readNibble();
    for (int bit = 0; bit < 4; bit++) {
      processBit(row, 0, bit, (col0 >> bit) & 1);
      processBit(row, 1, bit, (col1 >> bit) & 1);
    }
    // キーボード走査は1行ごとに待ち時間があるだけの遅い処理なので、その合間に
    // SFC(マウス)を読んでおくと、キーボードの走査速度を落とさずにマウスの
    // 更新頻度だけ約10倍に上げられる(カクつき対策)
    scanSFC();
  }
  firstScan = false;
}

void scanZapper() {
  bool trig = digitalRead(ZAPPER_TRIGGER);
  if (trig != lastTrigger) {
    toPico.write((trig == LOW ? 0x80 : 0x00) | 72); // id=72 トリガー
    lastTrigger = trig;
  }
  bool sens = digitalRead(ZAPPER_SENSOR);
  if (sens != lastSensor) {
    toPico.write((sens == LOW ? 0x80 : 0x00) | 73); // id=73 センサー
    lastSensor = sens;
  }
}

void sendSFCPadEvent(uint8_t baseId, int index, bool pressed) {
  toPico.write((pressed ? 0x80 : 0x00) | (baseId + index));
}

// 32bit丸ごと読んだ後、標準パッドかマウスかを判定して処理する。
// ホットスワップにも対応(毎スキャンごとに再判定)。
//
// 実機解析で確定した仕様(2026-08-02):
// - 信号線は全ビットがアクティブLowで伝送される(論理値の反転がそのまま生値になる)
// - マウスは生のbyte1下位4bitが常に0b1110固定(論理シグネチャ0001の反転)。
//   これはパッドの「先頭バイトが0x00かどうか」では判定できない
//   (パッドもボタン全解放時は先頭バイトが生0xFFになり、マウスの生0xFFと衝突するため)
// - L/Rボタンは(反転後の)byte1の上位2bit: bit7=Right、bit6=Left
// force=trueの時は「変化なし」でも現在押されているボタンを再送する(Pico側の
// ウォッチドッグが、押しっぱなし中に無通信と誤認して勝手に離してしまうのを防ぐため)
void processSFCPort(uint32_t report, uint8_t buttonBaseId, uint8_t mouseId,
                     bool &isMouse, bool padButtons[12], uint8_t &mouseButtons, bool force) {
  uint8_t rawByte1 = (report >> 16) & 0xFF;
  bool mouseReport = (rawByte1 & 0x0F) == 0x0E; // マウス固有シグネチャ(実測確定)
  if (mouseReport) {
    // マウスレポート
    if (!isMouse) {
      // パッドモードから切り替わった瞬間、押しっぱなしのボタンを強制解放
      for (int i = 0; i < 12; i++) {
        if (padButtons[i]) {
          sendSFCPadEvent(buttonBaseId, i, false);
          padButtons[i] = false;
        }
      }
      isMouse = true;
    }
    uint8_t byte1 = ~rawByte1 & 0xFF;               // bit7=Right bit6=Left bit5-4=感度 bit3-0=signature(0001)
    uint8_t byte2 = ~((report >> 8) & 0xFF) & 0xFF; // Y方向(bit7=符号, bit6-0=移動量)
    uint8_t byte3 = ~report & 0xFF;                 // X方向(bit7=符号, bit6-0=移動量)
    uint8_t newButtons = 0;
    if (byte1 & 0x40) newButtons |= 0x01; // Left  (実機確認済み)
    if (byte1 & 0x80) newButtons |= 0x02; // Right (実機確認済み)
    bool changed = (newButtons != mouseButtons);
    if (dx_or_dy_nonzero(byte2, byte3) || changed || (force && newButtons != 0)) {
      int8_t dy = (byte2 & 0x80) ? -(int8_t)(byte2 & 0x7F) : (int8_t)(byte2 & 0x7F);
      int8_t dx = (byte3 & 0x80) ? -(int8_t)(byte3 & 0x7F) : (int8_t)(byte3 & 0x7F);
      toPico.write(0x80 | mouseId);
      toPico.write(newButtons);
      toPico.write((uint8_t)dx);
      toPico.write((uint8_t)dy);
      mouseButtons = newButtons;
    }
  } else {
    // 標準パッドレポート(先頭16bit: B Y Select Start Up Down Left Right | A X L R 0 0 0 0)
    if (isMouse) {
      // マウスモードから切り替わった瞬間、マウスボタンを強制解放
      if (mouseButtons != 0) {
        toPico.write(0x80 | mouseId);
        toPico.write((uint8_t)0);
        toPico.write((uint8_t)0);
        toPico.write((uint8_t)0);
        mouseButtons = 0;
      }
      isMouse = false;
    }
    uint16_t pad16 = (report >> 16) & 0xFFFF;
    for (int i = 0; i < 12; i++) {
      bool pressed = ((pad16 >> (15 - i)) & 0x01) == 0; // アクティブLow
      if (pressed != padButtons[i] || (force && pressed)) {
        sendSFCPadEvent(buttonBaseId, i, pressed);
        padButtons[i] = pressed;
      }
    }
  }
}

bool dx_or_dy_nonzero(uint8_t byte2, uint8_t byte3) {
  return (byte2 & 0x7F) != 0 || (byte3 & 0x7F) != 0;
}

unsigned long lastSFCHeartbeat = 0;
const unsigned long SFC_HEARTBEAT_MS = 100; // Pico側WATCHDOG_TIMEOUT(0.3s)より十分短く

void scanSFC() {
  digitalWrite(SFC_LATCH, HIGH);
  delayMicroseconds(12);
  digitalWrite(SFC_LATCH, LOW);
  delayMicroseconds(6);

  uint32_t report1P = 0;
  uint32_t report2P = 0;
  for (int i = 0; i < 32; i++) {
    report1P = (report1P << 1) | digitalRead(SFC_DATA_1P);
    report2P = (report2P << 1) | digitalRead(SFC_DATA_2P);
    digitalWrite(SFC_CLOCK, LOW);
    delayMicroseconds(6);
    digitalWrite(SFC_CLOCK, HIGH);
    delayMicroseconds(6);
  }

  unsigned long now = millis();
  bool force = (now - lastSFCHeartbeat >= SFC_HEARTBEAT_MS);
  if (force) lastSFCHeartbeat = now;

  processSFCPort(report1P, SFC1_BUTTON_BASE, SFC1_MOUSE_ID, sfc1IsMouse, sfc1PadButtons, sfc1MouseButtons, force);
  processSFCPort(report2P, SFC2_BUTTON_BASE, SFC2_MOUSE_ID, sfc2IsMouse, sfc2PadButtons, sfc2MouseButtons, force);
}

void setup() {
  toPico.begin(57600);
  pinMode(OUT0, OUTPUT);
  pinMode(OUT1, OUTPUT);
  pinMode(OUT2, OUTPUT);
  pinMode(D1, INPUT_PULLUP);
  pinMode(D2, INPUT_PULLUP);
  pinMode(D3, INPUT_PULLUP);
  pinMode(D4, INPUT_PULLUP);
  pinMode(ZAPPER_TRIGGER, INPUT_PULLUP);
  pinMode(ZAPPER_SENSOR, INPUT_PULLUP);

  pinMode(SFC_CLOCK, OUTPUT);
  pinMode(SFC_LATCH, OUTPUT);
  digitalWrite(SFC_CLOCK, HIGH); // アイドル時Highが実機仕様
  digitalWrite(SFC_LATCH, LOW);
  pinMode(SFC_DATA_1P, INPUT_PULLUP); // 未接続時は標準パッドの「ボタン全部離し」として読める
  pinMode(SFC_DATA_2P, INPUT_PULLUP);
}

void loop() {
  scanKeyboard();
  scanZapper();
  scanSFC();
  // キーボード走査だけで約9ms(500us×18回)かかるので、末尾の待機は最小限にして
  // SFC(クリック/移動)の応答性を上げる
  delay(1);
}
