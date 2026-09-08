#include "misc_types.h"

#include "some_globals.h"
#include "aux_code.h"
#include "tpak_class.h"
#include "cart_helper_class.h"

// Per the N64brew wiki's Joybus Protocol page, the controller keeps every
// Transfer Pak read and write nullified (reads return 32 zero bytes,
// writes are dropped) until the software has sent the Info/identify
// command covering the pak's insertion. The status byte's low two bits
// report 1 = accessory inserted, 2 = none inserted, 3 = a *new* accessory
// since the last Info command.
//
// The original setup() sends identify exactly once at power-on, which
// misses that transition whenever the pak was already plugged in before
// the Arduino booted -- and then nothing ever reads back but zeros. So
// poll identify and wait to actually see the 2 -> 3 transition, which
// means physically pulling the pak out and pushing it back in while this
// is running. Power-cycling the pak through the controller's 3.3V line
// does not substitute: the controller comes back reporting 1, not 3.
//
// The onboard LED is the user's cue: blinking = "reinsert the pak",
// solid = "got a good one, hands off".
static const int LED_PIN = 13;

static void wait_for_pak_insertion()
{
	Serial.println("Remove and reinsert the Transfer Pak now (LED blinking).");

	uint8_t last_status = 0xFF;
	unsigned long start = millis();
	bool led = false;
	for (;;)
	{
		led = !led;
		digitalWrite(LED_PIN, led ? HIGH : LOW);

		unsigned char command[] = {0x00};
		clear_mem_dump();
		noInterrupts();
		N64_send(command, 1);
		N64_read_addr();
		interrupts();
		manage_mem_dump();

		uint8_t status = N64_mem_managed[2] & 0x3;
		if (status != last_status)
		{
			Serial.print("  status -> 0x");
			Serial.println(status, HEX);
			last_status = status;
		}
		if (status == 3)
		{
			digitalWrite(LED_PIN, LOW);
			Serial.println("Got the fresh-insertion transition. Settling...");
			delay(300); // let the pak settle before touching it
			return;
		}
		if (millis() - start > 30000)
		{
			Serial.println("Still waiting for a reinsertion...");
			start = millis();
		}
		delay(100);
	}
}

// Reads and writes fail independently, so check both before trusting an
// insertion:
//
//   Reads  -- the Nintendo logo at GB 0x0104 must come back as the fixed
//             CE ED 66 66 pattern every real cartridge starts with,
//             *and* the header checksum at GB 0x014D has to match the
//             header bytes. The logo alone is not enough: a marginal
//             insertion here left data bit 2 stuck high, and CE ED 66 66
//             all have bit 2 set already, so the logo check sailed
//             through while the header read back as 07 04 06 instead of
//             03 04 02 (every byte differing by exactly 0x04). The
//             checksum catches that.
//   Writes -- reading the same Transfer Pak address in bank 0 and bank 1
//             (GB 0x0000 vs 0x4000) must differ. When writes are dropped
//             the bank register never moves, so every "switched" bank
//             read returns bank 0 and the whole ROM dumps as N identical
//             copies of it: valid header, valid header checksum, wrong
//             global checksum.
static bool verify_cart_access(cart_helper *cart)
{
	cart->my_tpak.set_bank(0x00);
	clear_mem_dump();
	cart->my_tpak.read(0xC100); // GB 0x0100-0x011F
	manage_mem_dump();
	if (!(N64_mem_managed[4] == 0xCE && N64_mem_managed[5] == 0xED &&
		N64_mem_managed[6] == 0x66 && N64_mem_managed[7] == 0x66))
	{
		Serial.println("Nintendo logo did not read back correctly.");
		return false;
	}

	// Header checksum over GB 0x0134-0x014C, compared against the value
	// stored at GB 0x014D. Those bytes span the 0xC120 block (0x0134
	// onwards) and the 0xC140 block.
	uint8_t header[25]; // GB 0x0134-0x014C
	clear_mem_dump();
	cart->my_tpak.read(0xC120); // GB 0x0120-0x013F
	manage_mem_dump();
	memcpy(header, &N64_mem_managed[20], 12); // GB 0x0134-0x013F
	clear_mem_dump();
	cart->my_tpak.read(0xC140); // GB 0x0140-0x015F
	manage_mem_dump();
	memcpy(header + 12, N64_mem_managed, 13); // GB 0x0140-0x014C
	uint8_t sum = 0;
	for (uint8_t i = 0; i < 25; ++i)
	{
		sum = sum - header[i] - 1;
	}
	if (sum != N64_mem_managed[13]) // GB 0x014D
	{
		Serial.print("Header checksum mismatch (computed 0x");
		Serial.print(sum, HEX);
		Serial.print(", stored 0x");
		Serial.print(N64_mem_managed[13], HEX);
		Serial.println(") -- bad read.");
		return false;
	}

	clear_mem_dump();
	cart->my_tpak.read(0xC000);
	manage_mem_dump();
	uint8_t bank0[8];
	memcpy(bank0, N64_mem_managed, sizeof(bank0));

	cart->my_tpak.set_bank(0x01);
	clear_mem_dump();
	cart->my_tpak.read(0xC000);
	manage_mem_dump();
	bool writes_work = memcmp(bank0, N64_mem_managed, sizeof(bank0)) != 0;
	cart->my_tpak.set_bank(0x00);
	if (!writes_work)
	{
		Serial.println("Reads work but the bank-switch write did not take.");
		return false;
	}

	// Good insertion -- (re)read the header fields.
	clear_mem_dump();
	cart->my_tpak.read(0xC140);
	manage_mem_dump();
	cart->set_raw_data(N64_mem_managed[7], N64_mem_managed[8], N64_mem_managed[9]);
	cart->interpret_raw_data();
	return true;
}

cart_helper *g_cart = NULL;

void setup()
{
	memset( N64_raw_dump, 0, 33 );

	pinMode( LED_PIN, OUTPUT );

	// Communication with the controller on this pin. Don't remove these
	// lines, we don't want to push +5V to the controller.
	digitalWrite( N64_PIN, LOW );
	pinMode( N64_PIN, INPUT );

	Serial.begin(115200);
	Serial.setTimeout(2);

	// Pak slot contact is marginal enough that a usable insertion can
	// take several tries, so just keep asking until both checks pass.
	for (;;)
	{
		wait_for_pak_insertion();

		if (g_cart)
		{
			delete g_cart;
		}
		// cart_helper's constructor runs tpak::init(), which enables the
		// pak, sets access mode and reads the header -- immediately
		// after the insertion transition, with no identify polling in
		// between (polling identify again can re-lock things).
		g_cart = new cart_helper();

		if (verify_cart_access(g_cart))
		{
			digitalWrite(LED_PIN, HIGH); // solid: we're in business
			Serial.println();
			g_cart->print_cart_stuff();
			Serial.println("Ready. Send 'r' to dump the ROM.");
			return;
		}

		Serial.println("Cartridge access check failed on this insertion -- try again.");
	}
}

void loop()
{
	if (!Serial.available())
	{
		return;
	}
	int c = Serial.read();

	if (c == 'r')
	{
		// Raw ROM bytes framed by markers, so the host side is just:
		// wait for "Ready", send 'r', read <n> bytes.
		uint32_t expected = (uint32_t)g_cart->rom_size * 16384UL;
		Serial.print("GBROM_BEGIN ");
		Serial.println(expected);
		g_cart->dump_rom();
		Serial.println();
		Serial.println("GBROM_END");
	}
	else if (c == 'i')
	{
		g_cart->print_cart_stuff();
	}
	else if (c == 's')
	{
		// Transfer Pak status register. Bits: 7 = powered, 6 = no
		// cartridge detected, 3&2 = cartridge reset detected
		// (self-clearing on read), 0 = access mode enabled.
		clear_mem_dump();
		g_cart->my_tpak.read(0xB000);
		manage_mem_dump();
		uint8_t st = N64_mem_managed[0];
		Serial.print("status @0xB000 = 0x");
		Serial.print(st, HEX);
		Serial.print("  powered=");
		Serial.print((st & 0x80) ? 1 : 0);
		Serial.print(" no_cart=");
		Serial.print((st & 0x40) ? 1 : 0);
		Serial.print(" cart_reset=");
		Serial.print((st & 0x0C) ? 1 : 0);
		Serial.print(" access_mode=");
		Serial.println((st & 0x01) ? 1 : 0);
	}
	else if (c == 'h')
	{
		g_cart->my_tpak.set_bank(0x00);
		clear_mem_dump();
		g_cart->my_tpak.read(0xC140);
		manage_mem_dump();
		Serial.print("header block:");
		for (int i = 0; i < 16; ++i)
		{
			Serial.print(" ");
			Serial.print(N64_mem_managed[i], HEX);
		}
		Serial.println();
	}
}
