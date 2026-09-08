"""
Pulls a ROM off the board and checks it before you trust it.

    pip install pyserial
    python dump.py COM8 --baud 921600 -o mario.gb

Waits for the sketch to report a verified insertion, sends the dump
command, saves the bytes, then validates the result:

  * the Nintendo logo at 0x0104
  * the header checksum at 0x014D
  * the global checksum at 0x014E-0x014F, which covers every byte in the
    file -- this is the one that matters, and the only thing that catches
    a dump where bank switching silently failed
  * how many 16 KiB banks are actually distinct

Exits non-zero if the global checksum doesn't match, so it's safe to use
in a loop or a script.
"""

import argparse
import hashlib
import sys
import time

import serial

LOGO_HEAD = bytes([0xCE, 0xED, 0x66, 0x66])


def header_checksum(rom):
    total = 0
    for i in range(0x134, 0x14D):
        total = (total - rom[i] - 1) & 0xFF
    return total


def global_checksum(rom):
    total = 0
    for i, b in enumerate(rom):
        if i not in (0x14E, 0x14F):
            total = (total + b) & 0xFFFF
    return total


def title(rom):
    raw = rom[0x134:0x143]
    return raw.split(b"\x00")[0].decode("ascii", "replace")


def validate(rom):
    """Prints a verdict, returns True if the dump is trustworthy."""
    stored_global = (rom[0x14E] << 8) | rom[0x14F]
    computed_global = global_checksum(rom)
    computed_header = header_checksum(rom)
    banks = len(rom) // 16384
    distinct = len(
        {hashlib.md5(rom[i * 16384:(i + 1) * 16384]).digest() for i in range(banks)}
    )

    logo_ok = rom[0x104:0x108] == LOGO_HEAD
    header_ok = computed_header == rom[0x14D]
    global_ok = computed_global == stored_global

    print()
    print(f"  title            {title(rom)!r}")
    print(f"  size             {len(rom)} bytes ({len(rom) / 1024:.0f} KiB)")
    print(f"  cartridge type   0x{rom[0x147]:02X}")
    print(f"  Nintendo logo    {'OK' if logo_ok else 'MISMATCH'}")
    print(
        f"  header checksum  {'OK' if header_ok else 'MISMATCH'}"
        f"  (computed 0x{computed_header:02X}, stored 0x{rom[0x14D]:02X})"
    )
    print(
        f"  global checksum  {'OK' if global_ok else 'MISMATCH'}"
        f"  (computed 0x{computed_global:04X}, stored 0x{stored_global:04X})"
    )
    print(f"  distinct banks   {distinct}/{banks}")
    print(f"  md5              {hashlib.md5(rom).hexdigest()}")
    print()

    if not global_ok:
        print("BAD DUMP -- the global checksum covers every byte, so this file is")
        print("wrong somewhere. Reinsert the pak and dump again.")
        if distinct == 1 and banks > 1:
            print("Every bank is identical: the cartridge's mapper never switched.")
        return False

    print("Good dump.")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial port, e.g. COM8 or /dev/ttyUSB0")
    ap.add_argument("-o", "--out", default="dump.gb", help="output file")
    ap.add_argument("--baud", type=int, default=921600,
                    help="115200 for the AVR build, 921600 for the ESP32 build")
    ap.add_argument("--wait", type=int, default=300,
                    help="seconds to wait for a verified insertion")
    ap.add_argument("--board", choices=("esp32", "avr"), default="esp32",
                    help="how to reset the board: ESP32 pulses RTS with DTR held "
                         "low (raising DTR too would drop it into the bootloader), "
                         "AVR pulses DTR")
    ap.add_argument("--no-reset", action="store_true",
                    help="don't reset the board -- use this to dump again without "
                         "redoing the insertion dance")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 2
    # Both lines idle before opening, so opening can't reset anything on
    # its own; the reset below is deliberate.
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)

    if not args.no_reset:
        if args.board == "esp32":
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
        else:
            ser.dtr = True
            time.sleep(0.1)
            ser.dtr = False
        time.sleep(0.4)

    ser.reset_input_buffer()

    if not args.no_reset:
        print("Waiting for a verified insertion.")
        print("Reinsert the Transfer Pak while the LED is blinking; keep going "
              "until it goes solid.\n")
        seen = b""
        deadline = time.time() + args.wait
        while time.time() < deadline:
            chunk = ser.read(256)
            if chunk:
                seen += chunk
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
                if b"dump ROM" in seen:
                    break
        else:
            print("\nGave up waiting for a good insertion.")
            return 1

    ser.reset_input_buffer()
    ser.write(b"r")
    ser.flush()

    line = ser.readline()
    for _ in range(40):
        if b"GBROM_BEGIN" in line:
            break
        line = ser.readline()
    else:
        print("Never saw GBROM_BEGIN; is the board in the ready state?")
        return 1

    expected = int(line.split()[1])
    print(f"\nDumping {expected} bytes...")

    data = bytearray()
    started = time.time()
    last_report = 0
    while len(data) < expected and time.time() - started < 1800:
        chunk = ser.read(min(16384, expected - len(data)))
        if chunk:
            data.extend(chunk)
        if len(data) - last_report >= expected // 8:
            pct = 100.0 * len(data) / expected
            print(f"  {len(data)}/{expected} ({pct:.0f}%)")
            last_report = len(data)
    ser.close()

    elapsed = time.time() - started
    print(f"Received {len(data)} bytes in {elapsed:.1f} s")
    if len(data) != expected:
        print("Short read -- the transfer did not finish.")
        return 1

    with open(args.out, "wb") as f:
        f.write(data)
    print(f"Wrote {args.out}")

    return 0 if validate(bytes(data)) else 1


if __name__ == "__main__":
    sys.exit(main())
