#pragma once

#include <stdint.h>
#include "tpak.h"

// Port of cart_helper_class.h/.cpp + cart_helper_mbc_funcs.h. Only the
// dump paths the original actually implemented are ported (rom_only,
// MBC1, MBC5 ROM dumping; MBC5 RAM dumping) -- MBC2/MBC3 dumping and
// RAM-restore were incomplete/stubbed in the original too.
enum class mbc_type {
    rom_only,
    mbc1,
    mbc1_ram,
    mbc2,
    mbc3,
    mbc3_timer,
    mbc3_timer_ram,
    mbc3_ram,
    mbc5,
    mbc5_ram,
    unknown_mbc_type,
};

enum class ram_size_type {
    rs_none,
    rs_2,
    rs_8,
    rs_32,
    rs_128,
    rs_mbc2,
};

class cart_helper {
public:
    // Enables the Transfer Pak and reads the cartridge header to
    // determine MBC type / ROM size / RAM size.
    void init();

    // Prints the Transfer Pak's access-mode readback and a raw read of
    // the Nintendo logo area (GB addr 0x0104, a well-known fixed non-zero
    // pattern on every real cartridge) to Serial, for bring-up debugging.
    void print_debug_probe();

    // Confirms this insertion is actually usable, and re-reads the header
    // if so. Checks two things, because they fail independently:
    //   1. Reads work -- the Nintendo logo at GB 0x0104 reads back as the
    //      fixed CE ED 66 66 pattern every real cartridge starts with.
    //   2. Writes work -- switching the Transfer Pak to bank 1 actually
    //      changes what the same address reads back. When writes are
    //      silently ignored the bank register never moves, so a whole ROM
    //      dump comes out as N identical copies of bank 0: valid header,
    //      valid header checksum, wrong global checksum.
    bool verify_access();

    mbc_type get_mbc_type() const { return cart_mbc_type_; }
    ram_size_type get_ram_size() const { return ram_size_; }
    uint16_t get_rom_size_banks() const { return rom_size_; } // # of 16KB banks
    const char *mbc_type_name() const;
    uint32_t ram_size_bytes() const;

    // Dumps the whole ROM, writing raw bytes to Serial as each 32-byte
    // block is read. Returns the total number of bytes written.
    uint32_t dump_rom();

    // Dumps external cart RAM (if any), same streaming-to-Serial
    // approach. Returns the total number of bytes written (0 if the
    // cart has no supported external RAM).
    uint32_t dump_ram();

private:
    // Real Transfer Paks are known to be finicky -- retry each
    // enable/mode-set step until a readback confirms it actually stuck,
    // rather than assuming success after a single write like the
    // original AVR code did. Returns false if it never stuck within
    // `max_tries`.
    bool robust_enable_tpak(int max_tries = 20);
    bool robust_set_access_mode(uint8_t mode, int max_tries = 20);

    uint8_t raw_cart_type_ = 0, raw_rom_size_ = 0, raw_ram_size_ = 0;
    mbc_type cart_mbc_type_ = mbc_type::unknown_mbc_type;
    uint16_t rom_size_ = 0; // number of 16KB ROM banks
    ram_size_type ram_size_ = ram_size_type::rs_none;
    tpak tpak_;

    void interpret_raw_data();
    void set_rom_bank_for_test(uint16_t bank);

    uint8_t calc_tpak_bank(uint16_t gb_addr) const { return (uint8_t)(gb_addr >> 14); }
    uint16_t calc_tpak_addr(uint16_t gb_addr) const { return (uint16_t)(gb_addr | 0xc000); }
    void write_byte_with_gb_addr(uint16_t gb_addr, uint8_t data);

    uint32_t rom_only_dump_rom();
    uint32_t mbc1_dump_rom();
    uint32_t mbc5_dump_rom();
    uint32_t mbc5_dump_ram();

    void mbc1_set_rom_bank_lo(uint8_t bank);
    void mbc5_set_rom_bank(uint8_t bank, uint8_t hbit);
    void mbc5_set_ram_bank(uint8_t bank);
    void mbc5_enable_ram();
    void mbc5_disable_ram();
};
