#include "tpak.h"
#include "n64_types.h"
#include "joybus.h"

#include <string.h>

namespace {

// Direct port of calc_addr_crc() from the original aux_code.cpp.
uint16_t calc_addr_crc(uint16_t address)
{
    static const uint16_t xor_table[16] = {
        0x0, 0x0, 0x0, 0x0, 0x0, 0x15, 0x1F, 0x0B,
        0x16, 0x19, 0x07, 0x0E, 0x1C, 0x0D, 0x1A, 0x01,
    };
    uint16_t crc = 0;
    address &= ~0x1F;
    for (int i = 15; i >= 5; --i) {
        if ((address >> i) & 0x1) {
            crc ^= xor_table[i];
        }
    }
    crc &= 0x1F;
    return address | crc;
}

// Every real transaction on this bus produces a 32-byte-payload +
// 1-byte-CRC response (264 bits) -- the original always calls
// N64_read_addr() after N64_send() regardless of the command, so we do
// the equivalent here regardless of read vs. write.
const size_t kResponseBits = 33 * 8;

} // namespace

void tpak::send_and_drain(uint8_t *command, size_t len, uint8_t *out33)
{
    uint8_t response[33] = {0};
    joybus::transact(command, len, response, kResponseBits);
    if (out33) {
        memcpy(out33, response, 33);
    }
}

void tpak::enable_tpak()
{
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(0x8000);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memset(&command[3], 0x84, 32);
    send_and_drain(command, sizeof(command), nullptr);
}

void tpak::get_access_mode(uint8_t *out33)
{
    read(0xB000, out33);
}

void tpak::set_access_mode(uint8_t mode)
{
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(0xB000);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memset(&command[3], mode, 32);
    send_and_drain(command, sizeof(command), nullptr);
}

void tpak::set_bank(uint8_t bank)
{
    current_bank_ = bank;
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(0xA000);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memset(&command[3], bank, 32);
    send_and_drain(command, sizeof(command), nullptr);
}

void tpak::read(uint16_t addr, uint8_t *out33)
{
    uint8_t command[3];
    command[0] = 0x02;
    addr_packet pkt;
    pkt.w = calc_addr_crc(addr);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    send_and_drain(command, sizeof(command), out33);
}

void tpak::write(uint16_t addr, const uint8_t *data32)
{
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(addr);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memcpy(&command[3], data32, 32);
    send_and_drain(command, sizeof(command), nullptr);
}

void tpak::write_raw(uint16_t addr, const uint8_t *data32, uint8_t *out33)
{
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(addr);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memcpy(&command[3], data32, 32);
    send_and_drain(command, sizeof(command), out33);
}

uint8_t tpak::data_crc(const uint8_t *data32)
{
    uint8_t ret = 0;
    for (int i = 0; i <= 32; ++i) {
        for (int j = 7; j >= 0; --j) {
            int tmp = 0;
            if (ret & 0x80) {
                tmp = 0x85;
            }
            ret <<= 1;
            if (i < 32) {
                if (data32[i] & (0x01 << j)) {
                    ret |= 0x1;
                }
            }
            ret ^= tmp;
        }
    }
    return ret;
}

void tpak::write_byte(uint16_t addr, uint8_t data)
{
    uint8_t command[35];
    command[0] = 0x03;
    addr_packet pkt;
    pkt.w = calc_addr_crc(addr);
    command[1] = pkt.hi;
    command[2] = pkt.lo;
    memset(&command[3], data, 32);
    send_and_drain(command, sizeof(command), nullptr);
}
