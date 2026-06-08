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

enum Type { T2716 = 0, T2732, T2764 };          // 2 KB / 4 KB / 8 KB (A0..A10 / A11 / A12)
inline size_t sizeOf(Type t) { return t == T2716 ? 2048u : t == T2732 ? 4096u : 8192u; }

void   begin();                                  // claim the 5 GPIOs (no-op unless enabled)
bool   available();                              // reader enabled + ready
size_t readChip(Type t, uint8_t* buf, size_t bufLen);   // read the whole chip; bytes read or 0
bool   dumpToSD(Type t, const char* path);              // read chip -> raw .bin on the SD (/dumps/)

} // namespace epromdump
