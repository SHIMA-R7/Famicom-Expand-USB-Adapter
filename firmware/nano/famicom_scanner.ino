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

SoftwareSerial toPico(2, 3); // RX=D2, TX=D3(分圧経由でPico GP1へ)

bool stableState[9][2][4];
bool candidateState[9][2][4];
uint8_t candidateCount[9][2][4];
bool firstScan = true;
const uint8_t DEBOUNCE_THRESHOLD = 2;

bool lastTrigger = HIGH;
bool lastSensor = HIGH;

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

void setup() {
  toPico.begin(9600);
  pinMode(OUT0, OUTPUT);
  pinMode(OUT1, OUTPUT);
  pinMode(OUT2, OUTPUT);
  pinMode(D1, INPUT_PULLUP);
  pinMode(D2, INPUT_PULLUP);
  pinMode(D3, INPUT_PULLUP);
  pinMode(D4, INPUT_PULLUP);
  pinMode(ZAPPER_TRIGGER, INPUT_PULLUP);
  pinMode(ZAPPER_SENSOR, INPUT_PULLUP);
}

void loop() {
  scanKeyboard();
  scanZapper();
  delay(5);
}
