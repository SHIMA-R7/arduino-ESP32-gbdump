Fork notes
==========

This fork adds three things to fl4shk's original arduinogbdump:

1. **The missing Transfer Pak unlock step**, without which every read and
   write to the pak silently returns zeros.
2. **Arduino Uno support** (the original README lists it as "planned").
3. **An ESP32-C6 port** of the Joybus layer, using the RMT peripheral
   instead of cycle-counted AVR assembly.

Everything below was worked out on real hardware: an Arduino Uno, an N64
controller, two different Transfer Paks, and four cartridges (Super Mario
Land, Super Mario Land 2, Wario Land, Pokémon Gold).


The Transfer Pak unlock step
----------------------------

The original code sends the Joybus Info/identify command (`0x00`) once in
`setup()` and then goes straight to talking to the pak. That works only if
the controller happens to report the pak as *newly inserted* at that
moment. Otherwise every subsequent pak read comes back as 32 zero bytes,
and every write is silently dropped.

Per the [N64brew Joybus Protocol page](https://n64brew.dev/wiki/Joybus_Protocol),
the controller keeps pak reads and writes nullified until the software has
sent an Info command covering the insertion event. The status byte's low
two bits report:

| Value | Meaning |
|-------|---------|
| `1`   | An accessory is inserted |
| `2`   | No accessory inserted |
| `3`   | A *new* accessory was inserted since the last Info command |

So this fork polls identify in a loop and waits to actually observe the
`2 → 3` transition before touching the pak. In practice that means the
user has to physically pull the pak out and push it back in while the
sketch is polling. The onboard LED (pin 13) blinks while waiting and goes
solid once a usable insertion has been found.

Things that were tried and do **not** work as a substitute:

- **Power-cycling the pak/controller** through the 3.3V line (we switched
  it with a 2N2907 high-side switch). The controller does lose power —
  identify goes silent while the switch is open — but it comes back
  reporting status `1`, not `3`. "Newly inserted" tracks the physical
  insertion event, not power-up.
- **Sinking the data line** during the outage to defeat parasitic powering
  through the pull-up. Same result.


Verifying an insertion before trusting a dump
---------------------------------------------

Reads and writes fail *independently*, and a pak that reads but ignores
writes is the dangerous case: the Transfer Pak's bank register never
moves, so every "switched bank" read returns bank 0 again. The result is a
ROM file with a correct header, a correct *header* checksum, and 32 or 64
identical copies of bank 0 — which only shows up as a wrong **global**
checksum. We produced exactly that on the first Super Mario Land 2 dump
before adding this check.

So after each insertion the sketch verifies both directions before
declaring itself ready:

- **Reads**: the Nintendo logo at GB `0x0104` must read back as its fixed
  `CE ED 66 66 …` pattern, **and** the header checksum at GB `0x014D` must
  match. The logo alone is not enough — a marginal insertion here left
  data bit 2 stuck high, and `CE`, `ED`, `66`, `66` all have bit 2 set
  already, so the logo check passed while the header read back as
  `07 04 06` instead of `03 04 02` (every byte off by exactly `0x04`).
  The checksum catches that class of failure.
- **Writes**: reading the same Transfer Pak address in bank 0 and bank 1
  (GB `0x0000` vs `0x4000`) must return different data.

If either check fails, the LED goes back to blinking and asks for another
reinsertion. Pak slot contact turned out to be marginal on our hardware —
a good insertion took anywhere from one to a dozen tries — so the retry
loop is what makes this practical.


Other Uno-side changes
----------------------

- `some_globals.h`: `bufsize` reduced from 1072 to 300. The original does
  not fit in the Uno's 2 KiB of SRAM (`data section exceeds available
  space`); only the bulk RAM-restore path uses the larger buffer.
- `cart_helper_mbc_funcs.h`: added `mbc3_dump_rom()` and wired MBC3 into
  `dump_rom()`. It was declared but never defined, so MBC3 carts (Pokémon
  Gold/Silver/Crystal among them) fell through to the `default:` case and
  dumped nothing. It mirrors the MBC1 path: fixed bank 0 first, then write
  the bank number to GB `0x2000` and read GB `0x4000-0x7FFF` for each
  remaining bank.
- `arduinogbdump.pde` renamed to `.ino` and rewritten around the flow
  above. It now streams the ROM over serial framed by `GBROM_BEGIN <n>` /
  `GBROM_END` instead of using the original's `sm_*` command protocol, so
  a dump is just: wait for "Ready", send `r`, capture `n` bytes.

Verified dumps (global checksum matching the value stored in the ROM
header, which covers every byte of the file):

| Cartridge | MBC | Size | Global checksum |
|-----------|-----|------|-----------------|
| Pokémon Gold | MBC3 | 1 MiB | `0x8A70` ✓ |
| Super Mario Land 2 | MBC1 | 512 KiB | `0xA613` ✓ |
| Super Mario Land | MBC1 | 64 KiB | `0x5ECF` ✓ |
| Wario Land (Super Mario Land 3) | MBC1 | 512 KiB | `0xF4A5` ✓ |


The ESP32-C6 port
-----------------

`src/esp32c6_gbdump/` is a PlatformIO project (Arduino framework) that
reimplements the Joybus layer on an ESP32-C6. The upper layers (`tpak`,
`cart_helper`) are straight ports of the originals.

The interesting part is `joybus.cpp`. A direct translation of the AVR
bit-banging does not work on this chip: the original gets its precision
from hand-counted NOPs in inline assembly, and under the ESP32
Arduino/IDF framework there is no equivalent — even with interrupts
disabled, C function-call overhead (`gpio_get_level()`, reading the
timer) blows the 1 µs budget. Measured on hardware, a software polling
loop decoded low-phase durations scattered between 2 and 9 µs where the
protocol wants a clean 1 vs 3 µs split. So both directions use the RMT
peripheral instead, which generates and measures the waveform in
hardware.

Things that cost real debugging time here, in case they save someone else
some:

- **`rmt_transmit_config_t::flags.eot_level` must be 1.** It defaults to
  0, which leaves the line actively driven LOW after the transmission
  instead of released — indistinguishable from a real low pulse to
  anything reading the pin afterwards.
- **RMT RX does not guarantee `duration0` is the low phase.** It records
  alternating segments starting from whatever level the line was at when
  armed, so you have to check `level0` to find which duration is the
  active-low one. Getting this wrong inverts the decoded bits.
- **Give TX and RX their own GPIOs**, wired together externally to the
  same bus. Sharing one pin between an RMT TX and RX channel produced
  untrustworthy captures, and deleting the TX channel could knock the RX
  channel out of its enabled state (`rmt_receive` then fails with
  "channel not in enable state").
- **Reset the RX channel after a receive timeout.** If no edge ever
  arrives the hardware reception stays pending forever, and every later
  `rmt_receive()` fails until you cycle `rmt_disable`/`rmt_enable`.

Status: the Joybus layer is verified against real hardware — the
controller identify command returns a correct `0x05 0x00 0x01`, and
transmitted command bytes were confirmed bit-for-bit against the wire
(including the address CRC). The full cartridge dump path on the ESP32
side has **not** been run end to end; the unlock and verification logic
was added to match the Uno version after the Uno was the one hooked up.
