#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/gpio.h"

// Generic N64/GameCube "Joybus" (1-wire, half-duplex, bit-banged serial)
// driver for ESP32. Not specific to GB-cartridge-via-Transfer-Pak dumping:
// transact() just sends an arbitrary command byte sequence and returns an
// arbitrary response bit sequence, so the same call works for polling a
// plain N64 controller's buttons/stick (command 0x01), talking to a
// Controller Pak/Rumble Pak, or a Transfer Pak. Anyone porting brownan's
// Gamecube-N64-Controller or fl4shk's arduinogbdump (both AVR-only,
// bit-banged with cycle-counted assembly) to an ESP32 can reuse this file
// as-is.
//
// Both TX and RX use the RMT peripheral (precise, hardware-timed pulse
// generation/measurement, not subject to compiler/cache/ISR jitter the
// way a plain CPU busy-loop is on this chip -- see the long comment at
// the top of joybus.cpp for why a straight port of the AVR original's
// NOP-counted bit-banging doesn't hold up here despite the much faster
// clock).
//
// Wiring: TX and RX each get their *own* GPIO, wired together externally
// (a single jumper wire) to the same physical controller-port data line,
// plus an external ~1k pull-up to 3.3V on that shared node (same as the
// original arduinogbdump schematic -- required, not optional, since the
// ESP32's internal pull-up alone is far too weak against typical line
// capacitance for clean, fast-enough edges at 1us resolution). Using two
// GPIOs instead of one is deliberate: an RMT TX channel and RX channel
// sharing one GPIO on this chip/driver were observed to interact in ways
// that made captured data untrustworthy (and even tearing the TX channel
// down could knock the RX channel out of its enabled state); giving RX
// its own GPIO that a TX channel never touches sidesteps that entirely.
// The line is driven open-drain: "high" means released (pulled up
// externally), "low" means actively driven low.

namespace joybus {

// Must be called once before transact(). `tx_pin` and `rx_pin` must be
// different GPIOs, both wired to the same physical bus (see file header).
void init(gpio_num_t tx_pin, gpio_num_t rx_pin);

// Sends `cmd_len` bytes from `cmd` (MSB first per byte) followed by a
// single stop bit -- exactly like N64_send() in the original code -- then
// waits for a response and decodes it into `out_bits` bits, packed
// MSB-first into `out_bytes` (ceil(out_bits/8) bytes must fit in the
// buffer).
//
// `timeout_us` bounds how long we wait for the whole response to finish
// arriving. Returns the number of response bits actually decoded (0 on
// failure -- call last_status() for details).
size_t transact(const uint8_t *cmd, size_t cmd_len, uint8_t *out_bytes, size_t out_bits,
                 uint32_t timeout_us = 5000);

// Human-readable status of the most recent transact() call.
const char *last_status();

// Bring-up check that the RMT TX path actually drives the bus. Sends one
// deliberately slow pulse (milliseconds, not microseconds) while polling
// the RX pin with plain digitalRead, and reports how many samples came
// back low. A working link reads low for most of the pulse; zero lows
// means TX isn't reaching the wire even though a DC-level jumper test
// might pass.
int tx_pulse_test(int samples = 400);

// Samples the raw RX-pin line level `samples` times back-to-back and
// returns how many of those samples read HIGH. Useful to sanity-check
// idle level / wiring without going through the joybus protocol at all.
int sample_idle_high_count(int samples = 1000);

} // namespace joybus
