# Pinout / Protocol Reference

## Famicom expansion port (DA-15) pin assignment

Confirmed by measurement + WDIC / nesmd.nomaki.jp reference material.

| DA15 pin | Role | Nano pin |
|---|---|---|
| 1 | GND | GND |
| 2 | (unused / MIC?) | not connected |
| 3 | (unused / unknown) | not connected |
| 4 | Keyboard D4 / **Zapper TRIGGER** | D11 |
| 5 | Keyboard D3 / **Zapper SENSOR** | D10 |
| 6 | Keyboard D2 | D9 |
| 7 | Keyboard D1 | D8 |
| 9 | /OE2 (found unused) | D12 (unused) |
| 10 | OUT2 | D7 |
| 11 | OUT1 | D6 |
| 12 | OUT0 | D5 |
| 14 | /OE1 (found unused) | D13 (unused) |
| 15 | +5V | Nano 5V |

**Note:** pins 4 and 5 are shared between the keyboard and the Zapper (the real
hardware assumes only one is ever connected at a time). Do not connect both
the keyboard and the Zapper simultaneously.

## Keyboard scan protocol

- 3-bit column select on OUT0/OUT1/OUT2 (equivalent to `$4016`): reset, then
  column 0, then column 1, scanning 9 rows.
- Reset: `0b101` (OUT0=1, OUT1=0, OUT2=1)
- Column 0 select: `0b100`
- Column 1 select: `0b110`
- Wait `delayMicroseconds(500)` after each column select before reading
  (parasitic capacitance; 300us was unstable).
- Data lines D1-D4 are read together. Not pressed = 1, pressed = 0 (inverted
  logic).

## Zapper

- Trigger: DA15 pin 4 (Nano D11)
- Sensor (light detect): DA15 pin 5 (Nano D10)
- Both are digital, active LOW on press / light detected.
- Assumes a Zapper with a NJL7502L + LM393 + NE555N-style output stage
  already driving DA15 pins 4/5.

## Pico GPIO assignment

| GPIO | Function |
|---|---|
| GP0 | UART0 TX -> Nano D2 (RX), direct connection |
| GP1 | UART0 RX <- Nano D3 (TX), **through voltage divider** |
| GP2 | Alt indicator LED |
| GP3 | CapsLock indicator LED (mirrors host lock state) |
| GP4 | NumLock indicator LED (mirrors host lock state) |
| GP5 | Shift indicator LED |
| GP6 | Mode switch (push button to GND) |

GP7 was tried for the mode switch earlier and turned out to be flaky; GP6 is
the pin currently in use (see main README for history).

## Nano <-> Pico UART level shift

Only the Nano(5V) -> Pico(3.3V) direction needs a divider. Pico -> Nano can
be wired directly.

```
Nano D3 (TX) --[10k]--+--[22k]-- GND
                       |
                   Pico GP1 (RX)

Nano D2 (RX) ------------------------ Pico GP0 (TX)
```

Divider output: 5V * 22/(10+22) ~= 3.44V (within Pico's tolerance).

## UART frame format (Nano -> Pico)

1 byte per event.

```
bit7: 1 = press, 0 = release
bit6-0: id (0-73)
```

- id 0-71: keyboard matrix (`row*8 + col*4 + bit`, 9 rows x 2 cols x 4 bits)
- id 72: Zapper trigger
- id 73: Zapper sensor

Baud rate: fixed at 9600bps.

## Key map (id -> key)

`row0`-`row8`, each row has `col0` (low nibble) / `col1` (high nibble),
bit0-3 = D1-D4.

```
row0: F8, RETURN, [, ]        | KANA(hankaku/zenkaku), RSHIFT, YEN, STOP(backspace)
row1: F7, @, :, ;             | _, /, -, ^
row2: F6, O, L, K             | ., ,, P, 0
row3: F5, I, U, J             | M, N, 9, 8
row4: F4, Y, G, H             | B, V, 7, 6
row5: F3, T, R, D             | F, C, 5, 4
row6: F2, W, S, A             | X, Z, E, 3
row7: F1, ESC, Q, CTR(Ctrl)   | LSHIFT, GRPH(Alt), 1, 2
row8: CLR_HOME, UP, RIGHT, LEFT | DOWN, SPACE, DEL, INS
```

### JIS layout symbol key corrections (measured on real hardware)

USB HID Usage IDs used directly (raw integers where `adafruit_hid.Keycode`
has no matching constant):

| Key | HID Usage |
|---|---|
| `[` | RIGHT_BRACKET (0x30) |
| `]` | BACKSLASH (0x31) |
| `@` | LEFT_BRACKET (0x2F) |
| `:` | QUOTE (0x34) |
| `;` | SEMICOLON (0x33, unchanged) |
| `_` | 0x87 (INTERNATIONAL1, raw value, **unverified on hardware**) |
| `-` | MINUS (0x2D) |
| `^` | EQUALS (0x2E) |
| YEN | 0x89 (INTERNATIONAL3, raw value, **unverified on hardware**) |
| KANA | 0x35 (hankaku/zenkaku) |

### Special behavior

- **Shift/Alt/Ctrl**: all **toggle** (each press flips on/off, not
  press-and-hold).
- **RETURN**: automatically releases Shift.
- **LED indicators** (keyboard mode):
  - Shift LED / Alt LED: reflect their own toggle state.
  - Ctrl alone: Shift and Alt LEDs both blink together.
  - Ctrl+Shift: Shift LED solid, Alt LED blinks.
  - Ctrl+Alt: Alt LED solid, Shift LED blinks.
  - Ctrl+Shift+Alt: Shift/Alt LEDs blink alternately.
  - CapsLock/NumLock LEDs: mirror the host PC's actual lock state
    (`kbd.led_on(Keyboard.LED_CAPS_LOCK)` etc).
- **Watchdog**: after 0.3s with no UART traffic, any held keyboard keys are
  force-released (Shift/Alt/Ctrl toggle state is intentionally excluded from
  this).

### Zapper mode

- Pressing the mode switch toggles keyboard mode <-> Zapper mode.
- In Zapper mode: id 72 (trigger) -> left mouse click, id 73 (sensor) ->
  right mouse click.
- Switching modes force-releases any held key/mouse button from the previous
  mode.
- LEDs while idle in Zapper mode: Alt -> Shift -> CapsLock -> NumLock chase
  animation (0.15s/step). While the trigger is held, all 4 blink together.

### Known interaction between keyboard and Zapper wiring

Because the trigger/sensor lines share the same physical pins as keyboard
D4/D3, holding the trigger down long enough also causes the keyboard matrix
scan to see the same signal change on every row for that bit, generating a
burst of spurious keyboard-matrix events (ids ending in the D4 bit position).
Confirmed on real hardware 2026-08-01: this happens for the trigger line
(long presses), not observed for the sensor line (likely filtered out by the
matrix scan's debounce, since sensor transitions tend to be shorter). This is
harmless as long as the keyboard is not connected at the same time.
