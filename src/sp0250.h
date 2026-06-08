// sp0250.h — GI SP0250 LPC speech DSP core (Gottlieb System-80B Gen1 voice, e.g. Arena).
//
// Ported from MAME src/devices/sound/sp0250.cpp — license:BSD-3-Clause, copyright-holders:Olivier
// Galibert — stripped of the MAME device_t/sound_stream/devcb glue; only the integer LPC DSP remains.
// The BSD-3-Clause license is retained (see sp0250.cpp header) — commercial use OK with attribution.
//
// Output frame rate on the GTS80BS1 board = clock/312 = 3.12 MHz / 312 = 10 kHz. next() returns one
// voice sample in [-64..63]; the caller upsamples to the mix rate.
#pragma once
#include <stdint.h>

namespace sp0250 {
void reset();              // clear FIFO + LPC state
void feed(uint8_t d);      // push one byte into the 15-byte LPC frame FIFO (strobed by sound_control bit6)
int  next();               // one voice sample [-64..63] at the ~10 kHz frame rate
bool ready();              // FIFO has room (true until 15 bytes latched)
unsigned long feeds();     // debug: nombre d octets recus
}
