N64 controller as a keyboard
============================

Play with the N64 controller instead of dumping from it. The Arduino
streams the controller state over serial and a host-side Python script
turns it into keystrokes, so RetroArch (or anything else) sees a normal
keyboard.

An Uno can't be a USB HID device on its own, which is why this goes
through the host rather than presenting a real gamepad.


Building the sketch
-------------------

`n64_keyboard.ino` reuses the Joybus layer from the dumper, and the
Arduino IDE requires every source file to sit in the sketch folder. So
copy these in from `../arduinogbdump/` before compiling:

    aux_code.cpp  aux_code.h
    some_globals.cpp  some_globals.h
    misc_types.h
    tpak_class.cpp  tpak_class.h
    cart_helper_class.cpp  cart_helper_class.h
    cart_helper_enums.h  cart_helper_mbc_funcs.h

Wiring is the same as the dumper's. Unlike the Transfer Pak, polling the
controller itself needs no unlock dance -- command `0x01` answers
straight away.


Running the bridge
------------------

    pip install pyserial
    python n64_bridge.py COM10

The port argument is optional if only one serial device is attached. The
LED on the Arduino stays lit while it is streaming. Ctrl+C stops the
bridge and releases any held keys.

Default mapping, matching RetroArch's stock player 1 keyboard binds:

| N64 | RetroPad | Key |
|-----|----------|-----|
| D-pad / analog stick | D-pad | arrow keys |
| A | A | X |
| B | B | Z |
| Start | Start | Enter |
| Z | Select | Right Shift |
| L | Y | A |
| R | X | S |
| C-left | L | Q |
| C-right | R | W |

Edit `BUTTON_TO_KEY` in `n64_bridge.py` to change it. `STICK_THRESHOLD`
sets how far the stick has to move before it counts as a direction --
these sticks are ~25 years old and rarely rest at exactly centre.


Note on SendInput
-----------------

Keys go out as hardware scancodes through `SendInput`, which is what
games and RetroArch's input driver actually read.

Worth knowing if you adapt this: on 64-bit Windows the `INPUT` struct
must be 40 bytes, because its union is sized by `MOUSEINPUT` (32 bytes),
not by `KEYBDINPUT` (24). Declaring the union any smaller makes
`sizeof(INPUT)` 32, and then **every** `SendInput` call fails with
`ERROR_INVALID_PARAMETER` and silently presses nothing. That cost an
evening here, chasing input drivers and process elevation, because the
return value wasn't being checked. `send_scancode()` now raises if the
call doesn't take, and the struct size is asserted at import.
