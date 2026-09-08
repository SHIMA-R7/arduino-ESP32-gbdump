#include <Arduino.h>
#include "joybus.h"
#include "cart_helper.h"

// GPIO0 = TX (drives the N64 controller-port data line), GPIO1 = RX
// (reads it). Both are wired together with a jumper to the same physical
// data line, plus an external ~1k pull-up to 3.3V on that shared node.
// See the file header comment in joybus.h for why TX and RX get separate
// pins.
static const gpio_num_t JOYBUS_TX_PIN = GPIO_NUM_0;
static const gpio_num_t JOYBUS_RX_PIN = GPIO_NUM_1;

static cart_helper g_cart;

static void print_cart_info()
{
    Serial.println();
    Serial.println("=== Cartridge info ===");
    Serial.printf("MBC type:  %s\n", g_cart.mbc_type_name());
    Serial.printf("ROM size:  %u banks (%lu KB)\n", g_cart.get_rom_size_banks(),
                  (unsigned long)g_cart.get_rom_size_banks() * 16);
    Serial.printf("RAM size:  %lu bytes\n", (unsigned long)g_cart.ram_size_bytes());
    Serial.println("======================");
    Serial.println("Send 'r' to dump ROM, 'm' to dump RAM (if any).");
}

// Blocks until the controller reports the pak's fresh-insertion
// transition (identify status byte, low 2 bits == 3). Until that is
// observed, the controller nullifies every pak read and write -- see the
// README for the full story. The user has to physically remove and
// reinsert the pak while this is polling; power-cycling the controller
// does not reproduce the transition.
static bool wait_for_pak_insertion(uint32_t timeout_ms)
{
    Serial.println("Remove and reinsert the Transfer Pak now...");
    uint8_t last_status = 0xFF;
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        uint8_t cmd = 0x00;
        uint8_t response[4] = {0};
        joybus::transact(&cmd, 1, response, 24);
        uint8_t status = response[2] & 0x3;
        if (status != last_status) {
            Serial.printf("  status -> 0x%X\n", status);
            last_status = status;
        }
        if (status == 3) {
            Serial.println("Got the fresh-insertion transition.");
            delay(300);
            return true;
        }
        delay(100);
    }
    return false;
}

void setup()
{
    Serial.begin(115200);
    delay(2000); // give the USB CDC terminal time to attach
    Serial.println("arduinogbdump / ESP32-C6 port");

    joybus::init(JOYBUS_TX_PIN, JOYBUS_RX_PIN);

    // Reads and writes both have to be confirmed working before a dump
    // is trustworthy: a pak that reads but does not accept writes will
    // silently return bank 0 for every "switched" bank, producing a ROM
    // with a valid header and a wrong global checksum.
    for (;;) {
        if (!wait_for_pak_insertion(60000)) {
            Serial.println("Timed out waiting for a reinsertion, retrying...");
            continue;
        }
        g_cart.init();
        if (g_cart.verify_access()) {
            break;
        }
        Serial.println("Cartridge access check failed on this insertion -- try again.");
    }

    print_cart_info();
}

void loop()
{
    if (!Serial.available()) {
        return;
    }
    int c = Serial.read();

    if (c == 'r') {
        uint32_t expected = (uint32_t)g_cart.get_rom_size_banks() * 16384;
        Serial.printf("GBROM_BEGIN %lu\n", (unsigned long)expected);
        uint32_t written = g_cart.dump_rom();
        Serial.printf("\nGBROM_END %lu\n", (unsigned long)written);
    } else if (c == 'm') {
        uint32_t expected = g_cart.ram_size_bytes();
        Serial.printf("GBRAM_BEGIN %lu\n", (unsigned long)expected);
        uint32_t written = g_cart.dump_ram();
        Serial.printf("\nGBRAM_END %lu\n", (unsigned long)written);
    } else if (c == 'i') {
        print_cart_info();
    }
}
