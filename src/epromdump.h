// epromdump.h — optional EPROM-reader daughterboard driver (dump the user's OWN ROM -> SD).
// See EPROM_READER.md for the schematic. The ESP drives 2x 74HC595 (address + /CE + /OE) and reads
// 1x 74HC165 (data) over 5 GPIOs (board_config: PIN_EPR_*). Gated by EPROM_READER_ENABLE: the code
// always compiles, but begin() only claims the pins (and available() turns true) when it's set to 1
// and the daughterboard is fitted. No PC, no separate programmer — the legal "dump your own" path.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace epromdump {

// Standard EPROMs (A0..A12 + /CE + /OE), plus the Gottlieb 80/80A system mask ROMs U2/U3 (2332
// type, 4Kx8). A 2332 sits bottom-justified in the 2764-wired ZIF, which lands its three quirky
// pins on controllable 595 outputs (A11->Q13, CS1->Q14, CS2->Q11), so U2/U3 are read by FIRMWARE
// alone — no adapter, no 7404. CS polarity: U2 = pin20 HIGH + pin21 LOW ; U3 = both HIGH.
enum Type { T2716 = 0, T2732, T2764, T2332_U2, T2332_U3 };
inline size_t sizeOf(Type t) {                   // 2716=2K, 2764=8K, 2732 + both 2332 = 4K
  return (t == T2716) ? 2048u : (t == T2764) ? 8192u : 4096u;
}
inline bool is2332(Type t) { return t == T2332_U2 || t == T2332_U3; }

void   begin();                                  // claim the 5 GPIOs (no-op unless enabled)
bool   available();                              // reader enabled + ready
size_t readChip(Type t, uint8_t* buf, size_t bufLen);   // read the whole chip; bytes read or 0
bool   dumpToSD(Type t, const char* path);              // read chip -> raw .bin on the SD (/dumps/)

} // namespace epromdump
