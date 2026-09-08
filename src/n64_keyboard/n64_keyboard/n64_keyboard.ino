#include "misc_types.h"

#include "some_globals.h"
#include "aux_code.h"
#include "tpak_class.h"
#include "cart_helper_class.h"

// Streams the N64 controller's state to the PC so a host-side bridge can
// turn it into keyboard input. Unlike the Transfer Pak work in this repo,
// polling the controller itself needs no unlock dance -- command 0x01
// answers straight away.
//
// Frame format, 5 bytes, sent as fast as the poll loop runs (~60 Hz):
//
//   0xA5      sync byte
//   data1     A B Z Start Dup Ddown Dleft Dright   (bit 7 .. bit 0)
//   data2     0 0 L R Cup Cdown Cleft Cright
//   stick_x   signed, -128..127
//   stick_y   signed, -128..127
//
// 0xA5 can legitimately appear in the payload, so the host resyncs by
// checking that frames stay aligned rather than trusting one sync byte.

static const uint8_t SYNC_BYTE = 0xA5;

void setup()
{
	memset( N64_raw_dump, 0, 33 );

	pinMode( 13, OUTPUT );

	// Don't push +5V at the controller: the pin is only ever an input
	// (pulled up externally to 3.3V) or driven low.
	digitalWrite( N64_PIN, LOW );
	pinMode( N64_PIN, INPUT );

	Serial.begin(115200);

	// Wake the controller up, same as the original setup() did.
	unsigned char initialize = 0x00;
	noInterrupts();
	N64_send( &initialize, 1 );
	N64_read_addr();
	interrupts();

	digitalWrite( 13, HIGH ); // solid LED: streaming
}

void loop()
{
	unsigned char command[] = {0x01}; // get controller state
	noInterrupts();
	N64_send(command, 1);
	N64_get();
	interrupts();

	translate_raw_data();

	Serial.write(SYNC_BYTE);
	Serial.write(N64_status.data1);
	Serial.write(N64_status.data2);
	Serial.write((uint8_t)N64_status.stick_x);
	Serial.write((uint8_t)N64_status.stick_y);

	delay(16); // ~60 Hz
}
