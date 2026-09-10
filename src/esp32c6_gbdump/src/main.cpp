#include <Arduino.h>
#include "joybus.h"
#include "cart_helper.h"

// TX drives the N64 controller-port data line, RX reads it. They get
// separate pins (see joybus.h) wired together externally to the same
// physical data line, with an external ~1k pull-up to 3.3V on that node.
//
// Defaults per board: GPIO0/GPIO1 on the C6 DevKit, GPIO4/GPIO18 on a
// classic ESP32 DevKitC.
//
// The classic ESP32 has several pins that can't carry this bus, because
// the data line's pull-up would hold them high while the chip boots:
//
//   GPIO0   boot mode select
//   GPIO2   must be low or floating to enter download mode -- pulling it
//           high blocks uploads while the controller is connected
//   GPIO12  must be low at boot or the flash voltage is set wrong
//   GPIO15  must be high at boot
//   GPIO6-11 wired to the internal SPI flash
//
// GPIO4 and GPIO18 have no boot-time role. GPIO19/21/22/23 are equally
// fine. For RX specifically, the input-only pins (GPIO34-39) are an even
// safer choice: they physically cannot drive the bus, and the external
// pull-up covers their lack of an internal one.
#ifndef JOYBUS_TX_GPIO
#ifdef CONFIG_IDF_TARGET_ESP32
#define JOYBUS_TX_GPIO 4
#define JOYBUS_RX_GPIO 18
#else
#define JOYBUS_TX_GPIO 0
#define JOYBUS_RX_GPIO 1
#endif
#endif

static const gpio_num_t JOYBUS_TX_PIN = (gpio_num_t)JOYBUS_TX_GPIO;
static const gpio_num_t JOYBUS_RX_PIN = (gpio_num_t)JOYBUS_RX_GPIO;

// Status cue, matching what the Uno build does with its plain LED:
// blinking = "reinsert the pak", solid = "good insertion, hands off".
// Boards with an addressable LED get colour too (amber / green / red);
// a plain LED just goes on for any non-black colour.
//
// On the classic DevKitC the onboard LED is on GPIO2, which is only free
// for this because the bus deliberately avoids that pin (it is a
// strapping pin -- see the note above).
#ifdef RGB_BUILTIN
static const int RGB_LED_PIN = RGB_BUILTIN;
#define HAVE_RGB_LED 1
#else
#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 2
#endif
#endif

static void led_begin()
{
#ifndef HAVE_RGB_LED
    pinMode(STATUS_LED_GPIO, OUTPUT);
    digitalWrite(STATUS_LED_GPIO, LOW);
#endif
}

static void led(uint8_t r, uint8_t g, uint8_t b)
{
#ifdef HAVE_RGB_LED
    // neopixelWrite() drives the LED through the RMT peripheral, so it
    // holds an RMT TX channel for as long as that pin stays initialised
    // -- and with few enough channels joybus then can't get one ("no
    // free tx channels", every transaction failing). Hand the channel
    // straight back after each update.
    neopixelWrite(RGB_LED_PIN, r, g, b);
    rmtDeinit(RGB_LED_PIN);
#else
    digitalWrite(STATUS_LED_GPIO, (r || g || b) ? HIGH : LOW);
#endif
}

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
    Serial.println("Remove and reinsert the Transfer Pak now (LED blinking amber).");
    uint8_t last_status = 0xFF;
    uint32_t start = millis();
    bool on = false;
    while (millis() - start < timeout_ms) {
        on = !on;
        led(on ? 40 : 0, on ? 20 : 0, 0); // amber blink

        uint8_t cmd = 0x00;
        uint8_t response[4] = {0};
        size_t bits = joybus::transact(&cmd, 1, response, 24);
        uint8_t status = response[2] & 0x3;
        if (status != last_status) {
            // A healthy controller answers 0x05 0x00 0xNN. Printing the
            // whole reply makes a wiring or timing problem obvious --
            // otherwise a dead bus just shows up as status 0, which is
            // not a value the protocol ever legitimately reports.
            Serial.printf("  status -> 0x%X   (identify: %02X %02X %02X, "
                          "%u bits, %s)\n",
                          status, response[0], response[1], response[2],
                          (unsigned)bits, joybus::last_status());
            last_status = status;
        }
        if (status == 3) {
            led(0, 0, 0);
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
    // 921600 rather than 115200: a 4 MiB cartridge is ~6 minutes of
    // transfer at 115200, and the longer the dump runs the more chance
    // the (marginal) pak contact has to drop out mid-way. At 921600 the
    // same cartridge takes well under a minute.
    Serial.begin(921600);
    delay(2000); // give the USB CDC terminal time to attach
    Serial.println("arduinogbdump / ESP32 port");
    Serial.printf("Joybus TX=GPIO%d  RX=GPIO%d\n",
                  (int)JOYBUS_TX_PIN, (int)JOYBUS_RX_PIN);

    led_begin();

    // Wiring self-test, before any protocol runs. TX and RX are supposed
    // to be jumpered to the same node, so driving TX low has to pull RX
    // low with it, and releasing TX has to let the external pull-up take
    // RX high again. If RX doesn't follow, the jumper or the pull-up is
    // missing -- which otherwise shows up much later and much less
    // clearly, as an identify that captures no edges at all.
    pinMode(JOYBUS_RX_PIN, INPUT);
    pinMode(JOYBUS_TX_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(JOYBUS_TX_PIN, LOW);
    delayMicroseconds(200);
    int low_reading = digitalRead(JOYBUS_RX_PIN);
    digitalWrite(JOYBUS_TX_PIN, HIGH); // release; pull-up should win
    delayMicroseconds(200);
    int high_reading = digitalRead(JOYBUS_RX_PIN);
    pinMode(JOYBUS_TX_PIN, INPUT);

    Serial.printf("Wiring self-test: TX low -> RX %d, TX released -> RX %d  [%s]\n",
                  low_reading, high_reading,
                  (low_reading == 0 && high_reading == 1) ? "OK"
                      : "CHECK THE JUMPER AND PULL-UP");
    if (high_reading == 0) {
        Serial.println("  RX stays low with TX released: the ~1k pull-up to 3.3V "
                       "is missing or the line is shorted to ground.");
    } else if (low_reading == 1) {
        Serial.println("  RX stays high with TX driven low: TX and RX are not on "
                       "the same node -- check the jumper between them.");
    }

    joybus::init(JOYBUS_TX_PIN, JOYBUS_RX_PIN);

    // Second self-test, this time through RMT rather than plain GPIO: a
    // DC jumper test can pass while the RMT TX path still isn't reaching
    // the pin, and then everything downstream just sees a silent bus.
    int lows = joybus::tx_pulse_test();
    Serial.printf("RMT TX self-test: RX read low on %d/400 samples  [%s]\n",
                  lows, lows > 100 ? "OK" : "RMT TX IS NOT DRIVING THE PIN");

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
            led(0, 60, 0); // solid green: ready
            break;
        }
        led(60, 0, 0); // red: this insertion was no good
        Serial.println("Cartridge access check failed on this insertion -- try again.");
        delay(500);
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
