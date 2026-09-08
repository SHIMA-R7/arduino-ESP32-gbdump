"""
Turns the N64 controller stream from the Arduino into keyboard input, so
RetroArch (or anything else) can be played with it.

Keys are sent as hardware scancodes through SendInput, which is what games
and RetroArch's input driver actually read -- character-level injection
tends to get ignored by them.

Default mapping targets RetroArch's stock keyboard bindings for player 1:

    N64            RetroPad        Key
    ---            --------        ---
    D-pad/stick    D-pad           arrow keys
    A              A               X
    B              B               Z
    Start          Start           Enter
    Z              Select          Right Shift
    L              Y               A
    R              X               S
    C-left         L               Q
    C-right        R               W

Usage:  python n64_bridge.py [COM port]
Stop with Ctrl+C.
"""

import ctypes
import ctypes.wintypes as wintypes
import sys
import time

import serial
import serial.tools.list_ports

SYNC_BYTE = 0xA5

# Stick deflection past which we report a direction. The sticks on these
# controllers are ~25 years old and rarely centre at exactly 0, so this
# also has to clear the resting drift.
STICK_THRESHOLD = 40

# --- Windows SendInput plumbing ---------------------------------------

KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_EXTENDEDKEY = 0x0001
INPUT_KEYBOARD = 1


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class MOUSEINPUT(ctypes.Structure):
    """Only here so the union ends up the right size.

    INPUT's union is as large as its biggest member, MOUSEINPUT -- 32
    bytes on x64, where KEYBDINPUT is only 24. Sizing the union by hand
    (or off KEYBDINPUT) makes sizeof(INPUT) 32 instead of 40, and
    SendInput then rejects every call with ERROR_INVALID_PARAMETER
    without pressing anything.
    """

    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUTunion(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("union", _INPUTunion)]


_EXPECTED_INPUT_SIZE = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
if ctypes.sizeof(INPUT) != _EXPECTED_INPUT_SIZE:
    raise SystemExit(
        f"INPUT struct is {ctypes.sizeof(INPUT)} bytes, expected "
        f"{_EXPECTED_INPUT_SIZE}; SendInput would silently do nothing."
    )

_user32 = ctypes.WinDLL("user32", use_last_error=True)


def send_scancode(scancode, extended, keyup):
    flags = KEYEVENTF_SCANCODE
    if extended:
        flags |= KEYEVENTF_EXTENDEDKEY
    if keyup:
        flags |= KEYEVENTF_KEYUP
    inp = INPUT(
        type=INPUT_KEYBOARD,
        union=_INPUTunion(
            ki=KEYBDINPUT(wVk=0, wScan=scancode, dwFlags=flags, time=0, dwExtraInfo=None)
        ),
    )
    sent = _user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if sent != 1:
        raise OSError(
            f"SendInput failed (err {ctypes.get_last_error()}) for scancode 0x{scancode:02X}"
        )


# Set 1 scancodes. The arrow keys are the "extended" (grey) ones.
KEYS = {
    "up": (0x48, True),
    "down": (0x50, True),
    "left": (0x4B, True),
    "right": (0x4D, True),
    "z": (0x2C, False),
    "x": (0x2D, False),
    "a": (0x1E, False),
    "s": (0x1F, False),
    "q": (0x10, False),
    "w": (0x11, False),
    "enter": (0x1C, False),
    "rshift": (0x36, False),
}

# --- Controller bit layout --------------------------------------------
# data1: A B Z Start Dup Ddown Dleft Dright  (bit 7 down to bit 0)
# data2: 0 0 L R Cup Cdown Cleft Cright

D1 = {
    "A": 0x80,
    "B": 0x40,
    "Z": 0x20,
    "Start": 0x10,
    "Dup": 0x08,
    "Ddown": 0x04,
    "Dleft": 0x02,
    "Dright": 0x01,
}
D2 = {
    "L": 0x20,
    "R": 0x10,
    "Cup": 0x08,
    "Cdown": 0x04,
    "Cleft": 0x02,
    "Cright": 0x01,
}

BUTTON_TO_KEY = {
    "A": "x",        # RetroPad A
    "B": "z",        # RetroPad B
    "Start": "enter",
    "Z": "rshift",   # Select
    "L": "a",        # RetroPad Y
    "R": "s",        # RetroPad X
    "Cleft": "q",    # RetroPad L
    "Cright": "w",   # RetroPad R
    "Dup": "up",
    "Ddown": "down",
    "Dleft": "left",
    "Dright": "right",
}


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in serial.tools.list_ports.comports():
        if "Arduino" in (p.description or "") or "CH34" in (p.description or ""):
            return p.device
    ports = list(serial.tools.list_ports.comports())
    if len(ports) == 1:
        return ports[0].device
    raise SystemExit(
        "Could not pick a serial port automatically. Pass one, e.g.: "
        "python n64_bridge.py COM10"
    )


def main():
    port = find_port()
    print(f"Opening {port} ...")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(2)  # the board resets when the port opens
    ser.reset_input_buffer()
    print("Streaming. Press Ctrl+C to stop.\n")

    held = set()

    def set_key(name, down):
        if down and name not in held:
            sc, ext = KEYS[name]
            send_scancode(sc, ext, keyup=False)
            held.add(name)
        elif not down and name in held:
            sc, ext = KEYS[name]
            send_scancode(sc, ext, keyup=True)
            held.discard(name)

    try:
        buf = bytearray()
        last_line = ""
        while True:
            chunk = ser.read(64)
            if not chunk:
                continue
            buf.extend(chunk)

            # Pull out complete frames, resyncing if we drift.
            while len(buf) >= 5:
                if buf[0] != SYNC_BYTE:
                    del buf[0]
                    continue
                frame = bytes(buf[:5])
                del buf[:5]

                data1, data2 = frame[1], frame[2]
                stick_x = frame[3] - 256 if frame[3] > 127 else frame[3]
                stick_y = frame[4] - 256 if frame[4] > 127 else frame[4]

                pressed = set()
                for name, mask in D1.items():
                    if data1 & mask:
                        pressed.add(name)
                for name, mask in D2.items():
                    if data2 & mask:
                        pressed.add(name)

                # The stick drives the same arrow keys as the D-pad.
                wanted = {BUTTON_TO_KEY[b] for b in pressed if b in BUTTON_TO_KEY}
                if stick_x < -STICK_THRESHOLD:
                    wanted.add("left")
                elif stick_x > STICK_THRESHOLD:
                    wanted.add("right")
                if stick_y > STICK_THRESHOLD:
                    wanted.add("up")
                elif stick_y < -STICK_THRESHOLD:
                    wanted.add("down")

                for key in KEYS:
                    set_key(key, key in wanted)

                line = (
                    "buttons: " + (",".join(sorted(pressed)) if pressed else "-")
                    + f"   stick: {stick_x:4d},{stick_y:4d}"
                    + "   keys: " + (",".join(sorted(wanted)) if wanted else "-")
                )
                if line != last_line:
                    print(line.ljust(90), end="\r", flush=True)
                    last_line = line
    except KeyboardInterrupt:
        pass
    finally:
        for key in list(held):
            sc, ext = KEYS[key]
            send_scancode(sc, ext, keyup=True)
        ser.close()
        print("\nStopped, all keys released.")


if __name__ == "__main__":
    main()
