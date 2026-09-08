#pragma once

#include <stdint.h>

// Port of misc_types.h's addr_packet union. Both AVR and ESP32 (Xtensa &
// RISC-V) are little-endian, so this layout ports over unchanged: `lo` is
// the low byte, `hi` the high byte, and code sends `.hi` then `.lo` onto
// the (big-endian-on-the-wire) Joybus bus.
union addr_packet {
    uint16_t w;
    struct {
        uint8_t lo, hi;
    };
};
