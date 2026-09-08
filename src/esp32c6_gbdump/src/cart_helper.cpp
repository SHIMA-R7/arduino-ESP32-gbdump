#include "cart_helper.h"
#include "joybus.h"

#include <Arduino.h>
#include <string.h>

bool cart_helper::robust_enable_tpak(int max_tries)
{
    for (int attempt = 0; attempt < max_tries; ++attempt) {
        tpak_.enable_tpak();
        delay(50);
        uint8_t rb[33] = {0};
        tpak_.read(0x8000, rb);
        bool ok = true;
        for (int j = 0; j < 32; ++j) {
            if (rb[j] != 0x84) {
                ok = false;
                break;
            }
        }
        if (ok) {
            Serial.printf("enable_tpak() confirmed after %d attempt(s)\n", attempt + 1);
            return true;
        }
        delay(50);
    }
    Serial.printf("enable_tpak() never confirmed after %d attempts\n", max_tries);
    return false;
}

bool cart_helper::robust_set_access_mode(uint8_t mode, int max_tries)
{
    for (int attempt = 0; attempt < max_tries; ++attempt) {
        tpak_.set_access_mode(mode);
        delay(50);
        uint8_t rb[33] = {0};
        tpak_.get_access_mode(rb);
        bool ok = true;
        for (int j = 0; j < 32; ++j) {
            if (rb[j] != mode) {
                ok = false;
                break;
            }
        }
        if (ok) {
            Serial.printf("set_access_mode(0x%02X) confirmed after %d attempt(s)\n", mode, attempt + 1);
            return true;
        }
        delay(50);
    }
    Serial.printf("set_access_mode(0x%02X) never confirmed after %d attempts\n", mode, max_tries);
    return false;
}

void cart_helper::init()
{
    delay(255 + 150); // initial settle, same as the original tpak::init()

    robust_enable_tpak();
    robust_set_access_mode(0xFF);

    // Cartridge header lives at GB address 0x0100-0x014F; with the
    // Transfer Pak's ROM bank left at its post-enable default of 0, GB
    // address 0x0140 maps to tpak address 0xC140 (same math as
    // calc_tpak_addr()/calc_tpak_bank() for bank 0).
    tpak_.set_bank(0x00);
    uint8_t header[33] = {0};
    tpak_.read(0xC140, header);
    delay(100);

    // Within that 32-byte block: offset 7 = GB addr 0x147 (cartridge
    // type), 8 = 0x148 (ROM size), 9 = 0x149 (RAM size).
    raw_cart_type_ = header[7];
    raw_rom_size_ = header[8];
    raw_ram_size_ = header[9];
    interpret_raw_data();
}

bool cart_helper::verify_access()
{
    // 1. Reads: the Nintendo logo starts at GB 0x0104, so the 32-byte
    //    block at TPak 0xC100 (GB 0x0100-0x011F) has CE ED 66 66 at
    //    offsets 4..7.
    tpak_.set_bank(0x00);
    uint8_t block[33] = {0};
    tpak_.read(0xC100, block);
    if (!(block[4] == 0xCE && block[5] == 0xED && block[6] == 0x66 && block[7] == 0x66)) {
        Serial.println("verify: Nintendo logo did not read back correctly");
        return false;
    }

    // 2. Reads, harder: the header checksum at GB 0x014D has to match
    //    the header bytes. The logo alone lets stuck data bits through --
    //    CE, ED, 66 and 66 all have bit 2 set, so a bit-2-stuck-high read
    //    sails past the logo check and still hands back a corrupt header
    //    (seen on the Uno: 07 04 06 where the cartridge really says
    //    03 04 02). Everything downstream then dumps nonsense.
    uint8_t hdr[25]; // GB 0x0134-0x014C
    uint8_t blk20[33] = {0};
    tpak_.read(0xC120, blk20); // GB 0x0120-0x013F
    memcpy(hdr, &blk20[20], 12);
    uint8_t blk40[33] = {0};
    tpak_.read(0xC140, blk40); // GB 0x0140-0x015F
    memcpy(hdr + 12, blk40, 13);
    uint8_t sum = 0;
    for (int i = 0; i < 25; ++i) {
        sum = sum - hdr[i] - 1;
    }
    if (sum != blk40[13]) { // GB 0x014D
        Serial.printf("verify: header checksum mismatch (computed 0x%02X, stored 0x%02X)\n",
                      sum, blk40[13]);
        return false;
    }

    // 3. Writes: the same TPak address in bank 0 vs bank 1 is GB 0x0000
    //    vs GB 0x4000, which always differ on a real cartridge. If they
    //    match, the bank-select write never took.
    uint8_t b0[33] = {0};
    tpak_.set_bank(0x00);
    tpak_.read(0xC000, b0);
    uint8_t b1[33] = {0};
    tpak_.set_bank(0x01);
    tpak_.read(0xC000, b1);
    tpak_.set_bank(0x00);
    if (memcmp(b0, b1, 8) == 0) {
        Serial.println("verify: reads work but the bank-switch write did not take");
        return false;
    }

    // Header is trustworthy now, so interpret it.
    raw_cart_type_ = blk40[7];
    raw_rom_size_ = blk40[8];
    raw_ram_size_ = blk40[9];
    interpret_raw_data();

    // 4. Writes that go *through* the pak to the cartridge's own mapper.
    //    Step 3 only proves the Transfer Pak's bank register moves; the
    //    MBC lives behind it and can stay stuck while that still works.
    //    When it does, the cartridge sits on its power-up bank and every
    //    "switched" read returns that same window -- a whole 4 MiB dumped
    //    as 256 copies of bank 1, real-looking data with no header in it.
    //    So actually switch the cartridge between two banks and require
    //    the window to change.
    if (cart_mbc_type_ != mbc_type::rom_only && rom_size_ > 2) {
        uint8_t win1[33] = {0};
        uint8_t win2[33] = {0};

        set_rom_bank_for_test(1);
        tpak_.set_bank(0x01);
        tpak_.read(0xC000, win1); // GB 0x4000 with bank 1 selected

        set_rom_bank_for_test(2);
        tpak_.set_bank(0x01);
        tpak_.read(0xC000, win2); // GB 0x4000 with bank 2 selected

        tpak_.set_bank(0x00);
        if (memcmp(win1, win2, 32) == 0) {
            Serial.println("verify: cartridge MBC bank switch did not take "
                           "(reads fine, but the mapper is stuck)");
            return false;
        }
    }

    return true;
}

// Selects a ROM bank the way this cartridge's mapper expects, for the
// bank-switch check above. MBC1/MBC3 take the bank number at GB 0x2000;
// MBC5 splits it into a low byte there plus a high bit at GB 0x3000.
void cart_helper::set_rom_bank_for_test(uint16_t bank)
{
    switch (cart_mbc_type_) {
        case mbc_type::mbc5:
        case mbc_type::mbc5_ram:
            mbc5_set_rom_bank((uint8_t)(bank & 0xFF), (uint8_t)(bank >> 8));
            break;
        default:
            write_byte_with_gb_addr(0x2000, (uint8_t)bank);
            break;
    }
}

void cart_helper::print_debug_probe()
{
    // Verify the write path's 32-byte payload actually arrives intact,
    // independent of whether the control register "does" anything with
    // it: the real device computes and returns a CRC-8 of the data bytes
    // it received, which we can compare against our own calc of what we
    // meant to send.
    uint8_t payload[32];
    memset(payload, 0x84, sizeof(payload));
    uint8_t resp[33] = {0};
    tpak_.write_raw(0x8000, payload, resp);
    uint8_t expected_crc = tpak::data_crc(payload);
    Serial.printf("write CRC check: device returned 0x%02X, expected 0x%02X (%s)\n",
                  resp[32], expected_crc, resp[32] == expected_crc ? "MATCH" : "MISMATCH");

    tpak_.set_bank(0x00);
    uint8_t logo[33] = {0};
    tpak_.read(0xC104, logo); // GB addr 0x0104 = start of Nintendo logo
    Serial.print("logo area @0x0104 (32 bytes, expect a fixed non-zero pattern):");
    for (int i = 0; i < 32; ++i) {
        Serial.printf(" %02X", logo[i]);
    }
    Serial.println();
}

const char *cart_helper::mbc_type_name() const
{
    switch (cart_mbc_type_) {
        case mbc_type::rom_only: return "None (ROM only)";
        case mbc_type::mbc1:
        case mbc_type::mbc1_ram: return "MBC1";
        case mbc_type::mbc2: return "MBC2";
        case mbc_type::mbc3:
        case mbc_type::mbc3_timer:
        case mbc_type::mbc3_timer_ram:
        case mbc_type::mbc3_ram: return "MBC3";
        case mbc_type::mbc5:
        case mbc_type::mbc5_ram: return "MBC5";
        default: return "unknown";
    }
}

uint32_t cart_helper::ram_size_bytes() const
{
    switch (ram_size_) {
        case ram_size_type::rs_none: return 0;
        case ram_size_type::rs_2: return 2 * 1024;
        case ram_size_type::rs_8: return 8 * 1024;
        case ram_size_type::rs_32: return 32 * 1024;
        case ram_size_type::rs_128: return 128 * 1024;
        case ram_size_type::rs_mbc2: return 512; // 512 x 4-bit nibbles
        default: return 0;
    }
}

void cart_helper::interpret_raw_data()
{
    switch (raw_cart_type_) {
        case 0x00: cart_mbc_type_ = mbc_type::rom_only; break;
        case 0x01: cart_mbc_type_ = mbc_type::mbc1; break;
        case 0x02:
        case 0x03: cart_mbc_type_ = mbc_type::mbc1_ram; break;
        case 0x05:
        case 0x06: cart_mbc_type_ = mbc_type::mbc2; break;
        case 0x0f: cart_mbc_type_ = mbc_type::mbc3_timer; break;
        case 0x10: cart_mbc_type_ = mbc_type::mbc3_timer_ram; break;
        case 0x11: cart_mbc_type_ = mbc_type::mbc3; break;
        case 0x12:
        case 0x13: cart_mbc_type_ = mbc_type::mbc3_ram; break;
        case 0x19: cart_mbc_type_ = mbc_type::mbc5; break;
        case 0x1a:
        case 0x1b: cart_mbc_type_ = mbc_type::mbc5_ram; break;
        case 0x1c: cart_mbc_type_ = mbc_type::mbc5; break; // has rumble
        case 0x1d:
        case 0x1e: cart_mbc_type_ = mbc_type::mbc5_ram; break; // has rumble
        default: cart_mbc_type_ = mbc_type::unknown_mbc_type; break;
    }

    rom_size_ = 2 << raw_rom_size_;

    if (cart_mbc_type_ != mbc_type::mbc2) {
        switch (raw_ram_size_ & 0x03) {
            case 0x00: ram_size_ = ram_size_type::rs_none; break;
            case 0x01: ram_size_ = ram_size_type::rs_2; break;
            case 0x02: ram_size_ = ram_size_type::rs_8; break;
            case 0x03: ram_size_ = ram_size_type::rs_32; break;
        }
        if ((raw_ram_size_ & 0x03) == 0 && raw_ram_size_ == 0x04) {
            ram_size_ = ram_size_type::rs_128;
        }
    } else {
        ram_size_ = ram_size_type::rs_mbc2;
    }
}

void cart_helper::write_byte_with_gb_addr(uint16_t gb_addr, uint8_t data)
{
    tpak_.set_bank(calc_tpak_bank(gb_addr));
    tpak_.write_byte(calc_tpak_addr(gb_addr), data);
}

uint32_t cart_helper::dump_rom()
{
    switch (cart_mbc_type_) {
        case mbc_type::rom_only: return rom_only_dump_rom();
        case mbc_type::mbc1:
        case mbc_type::mbc1_ram: return mbc1_dump_rom();
        case mbc_type::mbc5:
        case mbc_type::mbc5_ram: return mbc5_dump_rom();
        default: return 0;
    }
}

uint32_t cart_helper::dump_ram()
{
    switch (cart_mbc_type_) {
        case mbc_type::mbc5:
        case mbc_type::mbc5_ram: return mbc5_dump_ram();
        default: return 0;
    }
}

namespace {
// Reads one 32-byte block at tpak address `addr` and streams it to
// Serial. Returns 32 (bytes written).
uint32_t dump_block(tpak &t, long addr)
{
    uint8_t block[33] = {0};
    t.read((uint16_t)addr, block);
    Serial.write(block, 32);
    return 32;
}
}

uint32_t cart_helper::rom_only_dump_rom()
{
    uint32_t total = 0;
    tpak_.set_bank(0x00);
    for (long j = 0xc000; j < 0x10000; j += 0x20) {
        total += dump_block(tpak_, j);
    }
    tpak_.set_bank(0x01);
    for (long j = 0xc000; j < 0x10000; j += 0x20) {
        total += dump_block(tpak_, j);
    }
    return total;
}

uint32_t cart_helper::mbc1_dump_rom()
{
    uint32_t total = 0;

    // First 16KB bank (fixed, GB addresses 0x0000-0x3fff).
    tpak_.set_bank(0x00);
    for (long j = 0xc000; j < 0x10000; j += 0x20) {
        total += dump_block(tpak_, j);
    }

    // Remaining switchable 16KB banks (GB addresses 0x4000-0x7fff).
    for (int i = 1; i < rom_size_; ++i) {
        mbc1_set_rom_bank_lo((uint8_t)i);
        tpak_.set_bank(0x01);
        for (long j = 0xc000; j < 0x10000; j += 0x20) {
            total += dump_block(tpak_, j);
        }
    }
    return total;
}

uint32_t cart_helper::mbc5_dump_rom()
{
    uint32_t total = 0;
    // MBC5 can map every 16KB bank into GB addresses 0x4000-0x7fff, so
    // there's no separate "fixed bank 0" pass needed.
    for (int i = 0; i < rom_size_; ++i) {
        mbc5_set_rom_bank((uint8_t)i, 0x00);
        tpak_.set_bank(0x01);
        for (long j = 0xc000; j < 0x10000; j += 0x20) {
            total += dump_block(tpak_, j);
        }
    }
    return total;
}

uint32_t cart_helper::mbc5_dump_ram()
{
    uint32_t total = 0;
    const long tpak_addr_start = 0xe000;

    switch (ram_size_) {
        case ram_size_type::rs_2: {
            mbc5_enable_ram();
            tpak_.set_bank(0x02);
            for (long j = tpak_addr_start; j < 0xe800; j += 0x20) {
                total += dump_block(tpak_, j);
            }
            mbc5_disable_ram();
            break;
        }
        case ram_size_type::rs_8: {
            mbc5_enable_ram();
            tpak_.set_bank(0x02);
            for (long j = tpak_addr_start; j < 0x10000; j += 0x20) {
                total += dump_block(tpak_, j);
            }
            mbc5_disable_ram();
            break;
        }
        case ram_size_type::rs_32: {
            for (uint8_t i = 0; i < 4; ++i) {
                mbc5_disable_ram();
                mbc5_set_ram_bank(i);
                mbc5_enable_ram();
                tpak_.set_bank(0x02);
                for (long j = tpak_addr_start; j < 0x10000; j += 0x20) {
                    total += dump_block(tpak_, j);
                }
            }
            mbc5_disable_ram();
            break;
        }
        case ram_size_type::rs_128: {
            for (uint8_t i = 0; i < 16; ++i) {
                mbc5_disable_ram();
                mbc5_set_ram_bank(i);
                mbc5_enable_ram();
                tpak_.set_bank(0x02);
                for (long j = tpak_addr_start; j < 0x10000; j += 0x20) {
                    total += dump_block(tpak_, j);
                }
            }
            mbc5_disable_ram();
            break;
        }
        default:
            break;
    }
    return total;
}

void cart_helper::mbc1_set_rom_bank_lo(uint8_t bank)
{
    write_byte_with_gb_addr(0x2000, bank);
}

void cart_helper::mbc5_set_rom_bank(uint8_t bank, uint8_t hbit)
{
    tpak_.set_bank(0x00);
    tpak_.write_byte(0xe000, bank);
    tpak_.write_byte(0xf000, hbit & 0x01);
}

void cart_helper::mbc5_set_ram_bank(uint8_t bank)
{
    write_byte_with_gb_addr(0x4000, bank & 0x0f);
}

void cart_helper::mbc5_enable_ram()
{
    write_byte_with_gb_addr(0x0000, 0x0a);
}

void cart_helper::mbc5_disable_ram()
{
    write_byte_with_gb_addr(0x0000, 0x00);
}
