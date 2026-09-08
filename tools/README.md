Host-side tools
===============

`gbdump_gui.py` — GB Dump Station
---------------------------------

One window for the whole workflow: flash the board, dump a cartridge,
check the result, and play with the controller.

    pip install pyserial
    python gbdump_gui.py

Plain Tkinter, no web view. To build the standalone executable:

    pip install pyinstaller
    pyinstaller --onefile --windowed --name GBDumpStation gbdump_gui.py

The .exe lands in `dist/`. It needs `arduino-cli` on PATH only for the
firmware section; dumping and the controller bridge work without it.

**Firmware** — compiles and uploads any of the three sketches. Each build
is staged into a temporary folder first, because none of the trees in
this repo are laid out the way `arduino-cli` wants (a folder containing a
`.ino` named after it, with every source beside it): the ESP32 one is a
PlatformIO project built around `main.cpp`, and the keyboard sketch needs
the dumper's shared Joybus sources. Staging keeps that out of the repo,
so nothing gets duplicated or left stale on disk.

Run the .exe from inside the repo (or leave it in `tools/dist/`) so it
can find `src/`. Dumping and the bridge work from anywhere.

**Dump ROM** — resets the board, waits for the sketch to report a
verified insertion, pulls the ROM with a progress bar, then validates it.
Reinsert the Transfer Pak while the board's LED blinks; keep going until
it goes solid. The result panel colours the checks green or red, and the
Save button writes the file with the cartridge's own title as the
suggested name.

The check that matters is the **global checksum**: it covers every byte
in the file, so it is the only one that catches a dump where bank
switching quietly failed and half the file is a copy of something else.
The logo and header checksum only prove the first few hundred bytes came
back intact.

**Controller bridge** — turns the N64 controller into keystrokes for
RetroArch (Windows only). Same mapping as `../src/n64_keyboard/`.

Board selection sets the baud rate (921600 for ESP32-C6, 115200 for AVR)
and, more importantly, how the board gets reset: the ESP32 pulses RTS
with DTR held low, since raising DTR as well would drop the chip into its
bootloader, while AVR boards pulse DTR.


`dump.py` — command line
------------------------

    python dump.py COM8 --board esp32 -o mario.gb
    python dump.py COM3 --board avr --baud 115200 -o tetris.gb

Same flow and same validation, printed to the terminal. Exits non-zero if
the global checksum doesn't match, so it can be scripted. `--no-reset`
dumps again without redoing the insertion dance, which is useful for
taking a second copy to compare against the first.
