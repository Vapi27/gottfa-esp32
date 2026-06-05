// soundlink.h — receives the FPGA's 1-wire sound link (see sound_link.vhd) and
// drives the WAV player: game# -> theme folder, sound# -> play. S3 sound tier.
// Protocol (8N1, one self-describing byte): 0x80|sound[4:0], 0x40|game[5:0].
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#ifndef BOARD_C3
namespace soundlink {
  void begin();   // open the UART (RX only) on PIN_SOUNDLINK_RX
  void poll();    // drain the UART FIFO -> wavplayer (call from loop())
}
#endif
