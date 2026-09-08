#pragma once

#include <stdint.h>
#include <stddef.h>

// Port of tpak_class.h/.cpp -- talks to an N64 Transfer Pak over the
// joybus link. All the addressing/CRC/command-format logic is identical
// to the original; only the underlying send+receive primitive changed
// (joybus::transact() instead of N64_send()+N64_read_addr()).
class tpak {
public:
    void enable_tpak();

    // Reads 33 bytes (32 data bytes + 1 CRC byte, CRC not validated here,
    // same as the original) into `out33`.
    void read(uint16_t addr, uint8_t *out33);

    void get_access_mode(uint8_t *out33);
    void set_access_mode(uint8_t mode);

    void set_bank(uint8_t bank);
    uint8_t get_bank() const { return current_bank_; }

    void write(uint16_t addr, const uint8_t *data32);
    void write_byte(uint16_t addr, uint8_t data);

    // Same as write(), but also hands back the 33-byte response (32
    // bytes + 1 CRC byte) instead of discarding it -- for diagnosing
    // whether the real device's returned CRC matches what data_crc()
    // computes for the payload we sent, which tells us whether the
    // write's 32 data bytes arrived intact independent of whatever the
    // control register does or doesn't do with them afterward.
    void write_raw(uint16_t addr, const uint8_t *data32, uint8_t *out33);

    // Direct port of calc_data_crc() from the original aux_code.cpp.
    static uint8_t data_crc(const uint8_t *data32);

private:
    void send_and_drain(uint8_t *command, size_t len, uint8_t *out33);
    uint8_t current_bank_ = 0;
};
