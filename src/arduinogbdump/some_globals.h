#ifndef some_globals_h
#define some_globals_h

#include "misc_types.h"

// 8 bytes of data that we get from the controller
extern N64_status_t N64_status;
extern char N64_raw_dump[33]; // 1 received bit per byte

static const word N64_mem_size = 264;
extern char N64_mem_dump[N64_mem_size+1];

static const word N64_byte_size = 33;
extern unsigned char N64_mem_managed[N64_byte_size];


// Buffer for Serial.readBytesUntil()
// Reduced from 1072 -- the original size doesn't fit in the Uno's 2KB of
// RAM (100% used, build fails with "data section exceeds available
// space"). This only affects sm_gb_write_var_num_bytes (bulk RAM
// restore); reads (cart detection / ROM dump), which is all we need for
// this diagnostic, are unaffected.
static const int bufsize = 300;
extern char buf[bufsize];



#endif		// some_globals_h
