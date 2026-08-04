import time
import board
import busio
import digitalio
import usb_hid
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keycode import Keycode
from adafruit_hid.mouse import Mouse

uart = busio.UART(board.GP0, board.GP1, baudrate=57600, timeout=0.01)
kbd = Keyboard(usb_hid.devices)
mouse = Mouse(usb_hid.devices)

mode_switch = digitalio.DigitalInOut(board.GP6)
mode_switch.direction = digitalio.Direction.INPUT
mode_switch.pull = digitalio.Pull.UP
zapper_mode = False
switch_last_state = True

shift_led = digitalio.DigitalInOut(board.GP5)
shift_led.direction = digitalio.Direction.OUTPUT
alt_led = digitalio.DigitalInOut(board.GP2)
alt_led.direction = digitalio.Direction.OUTPUT
capslock_led = digitalio.DigitalInOut(board.GP3)
capslock_led.direction = digitalio.Direction.OUTPUT
numlock_led = digitalio.DigitalInOut(board.GP4)
numlock_led.direction = digitalio.Direction.OUTPUT

HANZEN_ZENKAKU = 0x35

KEYMAP = [
    Keycode.F8, Keycode.ENTER, Keycode.RIGHT_BRACKET, Keycode.BACKSLASH,
    HANZEN_ZENKAKU, Keycode.RIGHT_SHIFT, 0x89, Keycode.BACKSPACE,
    Keycode.F7, Keycode.LEFT_BRACKET, Keycode.QUOTE, Keycode.SEMICOLON,
    0x87, Keycode.FORWARD_SLASH, Keycode.MINUS, Keycode.EQUALS,
    Keycode.F6, Keycode.O, Keycode.L, Keycode.K,
    Keycode.PERIOD, Keycode.COMMA, Keycode.P, Keycode.ZERO,
    Keycode.F5, Keycode.I, Keycode.U, Keycode.J,
    Keycode.M, Keycode.N, Keycode.NINE, Keycode.EIGHT,
    Keycode.F4, Keycode.Y, Keycode.G, Keycode.H,
    Keycode.B, Keycode.V, Keycode.SEVEN, Keycode.SIX,
    Keycode.F3, Keycode.T, Keycode.R, Keycode.D,
    Keycode.F, Keycode.C, Keycode.FIVE, Keycode.FOUR,
    Keycode.F2, Keycode.W, Keycode.S, Keycode.A,
    Keycode.X, Keycode.Z, Keycode.E, Keycode.THREE,
    Keycode.F1, Keycode.ESCAPE, Keycode.Q, Keycode.LEFT_CONTROL,
    Keycode.LEFT_SHIFT, Keycode.LEFT_ALT, Keycode.ONE, Keycode.TWO,
    Keycode.HOME, Keycode.UP_ARROW, Keycode.RIGHT_ARROW, Keycode.LEFT_ARROW,
    Keycode.DOWN_ARROW, Keycode.SPACE, Keycode.BACKSPACE, Keycode.INSERT,
]

SHIFT_CODES = (Keycode.LEFT_SHIFT, Keycode.RIGHT_SHIFT)
ALT_CODES = (Keycode.LEFT_ALT, Keycode.RIGHT_ALT)
CTRL_CODES = (Keycode.LEFT_CONTROL, Keycode.RIGHT_CONTROL)

# SFCコントローラー: id 74-85(1P)/86-97(2P)、並びはB,Y,Select,Start,Up,Down,Left,Right,A,X,L,R
# 押しっぱなし方式(トグルではない)。好みに応じて自由に差し替え可。
SFC1_BUTTON_BASE = 74
SFC2_BUTTON_BASE = 86
SFC1_MOUSE_ID = 98
SFC2_MOUSE_ID = 99
# SFCマウス純正の分解能(感度0で50カウント/インチ程度)は今どきのUSBマウスよりかなり
# 粗いので、そのまま送ると動きが鈍く感じる。ここで倍率をかけて感度を底上げする。
SFC_MOUSE_GAIN = 24

KEYMAP_SFC1 = [
    Keycode.Z, Keycode.A, Keycode.RIGHT_SHIFT, Keycode.ENTER,
    Keycode.UP_ARROW, Keycode.DOWN_ARROW, Keycode.LEFT_ARROW, Keycode.RIGHT_ARROW,
    Keycode.X, Keycode.S, Keycode.Q, Keycode.W,
]
KEYMAP_SFC2 = [
    Keycode.KEYPAD_ONE, Keycode.KEYPAD_SEVEN, Keycode.KEYPAD_ZERO, Keycode.KEYPAD_ENTER,
    Keycode.KEYPAD_EIGHT, Keycode.KEYPAD_TWO, Keycode.KEYPAD_FOUR, Keycode.KEYPAD_SIX,
    Keycode.KEYPAD_THREE, Keycode.KEYPAD_NINE, Keycode.KEYPAD_MINUS, Keycode.KEYPAD_PLUS,
]

last_packet_time = time.monotonic()
WATCHDOG_TIMEOUT = 0.3
shift_active = False
alt_active = False
ctrl_active = False
pressed_keys = set()
trigger_held = False

# SFCコントローラー/マウスはキーボードモード・ザッパーモードどちらでも常時動作する
# (ファミコン側とSFC側は配線・信号とも独立しているため同時利用可能)
sfc_pressed_keys = set()
sfc1_mouse_buttons = 0
sfc2_mouse_buttons = 0
sfc_last_packet_time = time.monotonic()

BLINK_STEP_DURATION = 0.25
blink_step_start = time.monotonic()
blink_phase = 0

# ==== ザッパーモード用アニメーション(alt→shift→capslock→numlock) ====
ZAPPER_ANIM_STEP = 0.15
zapper_anim_start = time.monotonic()
zapper_anim_index = 0
ZAPPER_ANIM_LEDS = [alt_led, shift_led, capslock_led, numlock_led]


def release_shift():
    global shift_active
    if shift_active:
        shift_active = False
        kbd.release(Keycode.LEFT_SHIFT)


def release_all_modifiers_and_keys():
    global shift_active, alt_active, ctrl_active
    for c in list(pressed_keys):
        kbd.release(c)
    pressed_keys.clear()
    if shift_active:
        kbd.release(Keycode.LEFT_SHIFT)
        shift_active = False
    if alt_active:
        kbd.release(Keycode.LEFT_ALT)
        alt_active = False
    if ctrl_active:
        kbd.release(Keycode.LEFT_CONTROL)
        ctrl_active = False


def update_leds_keyboard_mode():
    global blink_step_start, blink_phase

    now = time.monotonic()
    if now - blink_step_start >= BLINK_STEP_DURATION:
        blink_phase = 1 - blink_phase
        blink_step_start = now
    blink_on = (blink_phase == 0)

    if not ctrl_active:
        shift_led.value = shift_active
        alt_led.value = alt_active
    elif shift_active and alt_active:
        shift_led.value = blink_on
        alt_led.value = not blink_on
    elif shift_active and not alt_active:
        shift_led.value = True
        alt_led.value = blink_on
    elif alt_active and not shift_active:
        shift_led.value = blink_on
        alt_led.value = True
    else:
        shift_led.value = blink_on
        alt_led.value = blink_on

    capslock_led.value = kbd.led_on(Keyboard.LED_CAPS_LOCK)
    numlock_led.value = kbd.led_on(Keyboard.LED_NUM_LOCK)


def update_leds_zapper_mode():
    global zapper_anim_start, zapper_anim_index

    if trigger_held:
        # トリガー中は全部同時点滅
        now = time.monotonic()
        if now - zapper_anim_start >= ZAPPER_ANIM_STEP:
            zapper_anim_index = 1 - zapper_anim_index
            zapper_anim_start = now
        on = (zapper_anim_index == 0)
        for led in ZAPPER_ANIM_LEDS:
            led.value = on
    else:
        # 待機中はalt→shift→capslock→numlockの順に流れる
        now = time.monotonic()
        if now - zapper_anim_start >= ZAPPER_ANIM_STEP:
            zapper_anim_index = (zapper_anim_index + 1) % len(ZAPPER_ANIM_LEDS)
            zapper_anim_start = now
        for i, led in enumerate(ZAPPER_ANIM_LEDS):
            led.value = (i == zapper_anim_index)


print("keyboard/zapper bridge start")

while True:
    try:
        sw = mode_switch.value
        if sw != switch_last_state:
            if sw == False:
                zapper_mode = not zapper_mode
                if zapper_mode:
                    release_all_modifiers_and_keys()
                else:
                    mouse.release_all()
                    trigger_held = False
                zapper_anim_index = 0
                zapper_anim_start = time.monotonic()
            switch_last_state = sw

        data = uart.read(1)
        if data:
            packet = data[0]
            pressed = bool(packet & 0x80)
            key_id = packet & 0x7F
            last_packet_time = time.monotonic()
            sfc_last_packet_time = last_packet_time

            if key_id == 72:  # トリガー
                if zapper_mode:
                    trigger_held = pressed
                    if pressed:
                        mouse.press(Mouse.LEFT_BUTTON)
                    else:
                        mouse.release(Mouse.LEFT_BUTTON)

            elif key_id == 73:  # センサー
                if zapper_mode:
                    if pressed:
                        mouse.press(Mouse.RIGHT_BUTTON)
                    else:
                        mouse.release(Mouse.RIGHT_BUTTON)

            elif SFC1_BUTTON_BASE <= key_id < SFC1_BUTTON_BASE + 12:
                # キーボード/ザッパーのモードに関係なく常時有効
                code = KEYMAP_SFC1[key_id - SFC1_BUTTON_BASE]
                if pressed:
                    kbd.press(code)
                    sfc_pressed_keys.add(code)
                else:
                    kbd.release(code)
                    sfc_pressed_keys.discard(code)

            elif SFC2_BUTTON_BASE <= key_id < SFC2_BUTTON_BASE + 12:
                code = KEYMAP_SFC2[key_id - SFC2_BUTTON_BASE]
                if pressed:
                    kbd.press(code)
                    sfc_pressed_keys.add(code)
                else:
                    kbd.release(code)
                    sfc_pressed_keys.discard(code)

            elif key_id in (SFC1_MOUSE_ID, SFC2_MOUSE_ID):
                # ヘッダーに続けてbuttons/dx/dyの3バイトが来る
                payload = uart.read(3)
                if payload and len(payload) == 3:
                    buttons = payload[0]
                    dx = payload[1] - 256 if payload[1] > 127 else payload[1]
                    dy = payload[2] - 256 if payload[2] > 127 else payload[2]
                    # 1回のHIDレポートは±127までしか送れないので、ゲインをかけた後の
                    # 総移動量が127を超える分は複数回に分けて送る(単純にクランプすると
                    # 速い動きが軒並み同じ最大値に潰れてカクついて見えるため)
                    total_dx = dx * SFC_MOUSE_GAIN
                    total_dy = dy * SFC_MOUSE_GAIN
                    while total_dx != 0 or total_dy != 0:
                        step_dx = max(-127, min(127, total_dx))
                        step_dy = max(-127, min(127, total_dy))
                        mouse.move(step_dx, step_dy, 0)
                        total_dx -= step_dx
                        total_dy -= step_dy
                    if key_id == SFC1_MOUSE_ID:
                        changed = buttons ^ sfc1_mouse_buttons
                        sfc1_mouse_buttons = buttons
                    else:
                        changed = buttons ^ sfc2_mouse_buttons
                        sfc2_mouse_buttons = buttons
                    if changed & 0x01:
                        (mouse.press if buttons & 0x01 else mouse.release)(Mouse.LEFT_BUTTON)
                    if changed & 0x02:
                        (mouse.press if buttons & 0x02 else mouse.release)(Mouse.RIGHT_BUTTON)

            elif not zapper_mode and key_id < len(KEYMAP):
                code = KEYMAP[key_id]
                if code is not None:
                    if code in SHIFT_CODES:
                        if pressed:
                            shift_active = not shift_active
                            if shift_active:
                                kbd.press(Keycode.LEFT_SHIFT)
                            else:
                                kbd.release(Keycode.LEFT_SHIFT)
                    elif code in ALT_CODES:
                        if pressed:
                            alt_active = not alt_active
                            if alt_active:
                                kbd.press(Keycode.LEFT_ALT)
                            else:
                                kbd.release(Keycode.LEFT_ALT)
                    elif code in CTRL_CODES:
                        if pressed:
                            ctrl_active = not ctrl_active
                            if ctrl_active:
                                kbd.press(Keycode.LEFT_CONTROL)
                            else:
                                kbd.release(Keycode.LEFT_CONTROL)
                    else:
                        if pressed:
                            kbd.press(code)
                            pressed_keys.add(code)
                            if code == Keycode.ENTER:
                                release_shift()
                        else:
                            kbd.release(code)
                            pressed_keys.discard(code)

        if not zapper_mode and time.monotonic() - last_packet_time > WATCHDOG_TIMEOUT and pressed_keys:
            for c in list(pressed_keys):
                kbd.release(c)
            pressed_keys.clear()
            last_packet_time = time.monotonic()

        if time.monotonic() - sfc_last_packet_time > WATCHDOG_TIMEOUT and (sfc_pressed_keys or sfc1_mouse_buttons or sfc2_mouse_buttons):
            for c in list(sfc_pressed_keys):
                kbd.release(c)
            sfc_pressed_keys.clear()
            if sfc1_mouse_buttons or sfc2_mouse_buttons:
                mouse.release_all()
                sfc1_mouse_buttons = 0
                sfc2_mouse_buttons = 0
            sfc_last_packet_time = time.monotonic()

        if zapper_mode:
            update_leds_zapper_mode()
        else:
            update_leds_keyboard_mode()

    except Exception as e:
        print("ERROR:", e)
        try:
            release_all_modifiers_and_keys()
            mouse.release_all()
            for c in list(sfc_pressed_keys):
                kbd.release(c)
            sfc_pressed_keys.clear()
            sfc1_mouse_buttons = 0
            sfc2_mouse_buttons = 0
        except Exception:
            pass
        last_packet_time = time.monotonic()
        sfc_last_packet_time = time.monotonic()
