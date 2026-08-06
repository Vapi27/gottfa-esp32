#pragma once
#include <Arduino.h>

// ============================================================================
//  Arena Wall-Art LED — configuration
//  Gottlieb "Arena" playfield turned into an illuminated wall decoration.
//  Target MCU: WEMOS/LOLIN D1 Mini ESP32 (also ESP32-S3 DevKitC-1 / ESP32-C3),
//  driving up to 150 SK6812MINI-RGBW on one data chain.
//  Decorative only: no gameplay electronics, no FPGA, no SPI bridge.
//  Full hardware/build notes: ../ARENA_LED.md
// ============================================================================

#define ARENA_FW_NAME    "Arena Wall-Art LED"
#define ARENA_FW_VERSION "1.0.1"
#define ARENA_MDNS_HOST  "arena"            // -> http://arena.local/

// ---- WiFi -------------------------------------------------------------------
// Leave the STA fields empty to boot straight into the SoftAP (join 'Arena-LED',
// password below, then open http://192.168.4.1/). Credentials set from the web UI
// are stored in NVS and win over these compile-time defaults.
#define ARENA_STA_SSID       ""
#define ARENA_STA_PASS       ""
#define ARENA_STA_TIMEOUT_MS 12000
#define ARENA_AP_SSID        "Arena-LED"
#define ARENA_AP_PASS        "pinball87"    // >= 8 chars

// ---- LED chain --------------------------------------------------------------
// One single data chain: ESP32 -> LED1 -> LED2 -> ... -> LEDn (data only; +5V and
// GND come from the two thick bus wires that thread through each board's slot
// pads, never through the data hops). LED_MAX sizes the frame buffers at compile time, so the
// firmware is built for the full 150-LED target from day one; LED_COUNT_DEFAULT is
// just the boot value and is changed live from the web UI (persisted in NVS).
#define LED_MAX              150
#define LED_COUNT_DEFAULT    100
#define LED_FRAME_HZ          60   // render/refresh rate (150 px RGBW = 4.8 ms/frame on the wire)

// Data pin, per target. Any output-capable GPIO works (the chain is driven by the
// RMT peripheral), but the choice has to dodge each chip's reserved pins.
#if defined(ARENA_BOARD_D1MINI32)
// --- WEMOS/LOLIN "D1 Mini ESP32" (ESP32-WROOM-32, 4 MB, CH340C, micro-USB) ---
// Broken out: 0,1,2,3,4,5,12..19,21..27,32..39. Avoid: 6-11 (flash), 34-39
// (input only), 12 (flash-voltage strap — pulling it high at reset browns out the
// flash), 0/2/5/15 (strapping), 1/3 (USB serial), 16/17 (PSRAM on WROVER modules).
// GPIO27 is free of all of that. Alternatives if 27 is taken: 25, 26, 32, 33, 14, 13, 4.
#define PIN_LED_DATA          27
#define PIN_LED_DATA2         26
#elif defined(BOARD_C3)
#define PIN_LED_DATA           5
#define PIN_LED_DATA2          6
#else
// --- ESP32-S3 DevKitC-1: avoid strapping (0,3,45,46), USB (19,20), flash/PSRAM (26-37) ---
// GPIO5 was the first choice. On the bench N16R8 it drove nothing: the firmware
// rendered frames but the chain stayed dark. The cause was never isolated (the
// board was off USB, so no serial evidence) - do not trust the octal-PSRAM
// explanation that was guessed here earlier, it was never measured.
// GPIO16 is clear of flash, PSRAM, strapping and USB on this module.
#define PIN_LED_DATA          16
#define PIN_LED_DATA2          6
#endif

// Second (optional) chain — split the playfield in two halves if one long run
// picks up too much noise. 0 = disabled (single chain, the documented build).
#define LED_CHAIN2_ENABLE      0

// ---- 3.3 V logic into a 5 V chain, without a 74AHCT125 ----------------------
// The SK6812 wants VIH >= 0.7 x VDD; at VDD = 5.0 V that is 3.5 V and an ESP32
// GPIO only reaches 3.3 V. Two hardware fixes need no logic IC (ARENA_LED.md §4):
//   A. run the whole chain at 4.3-4.5 V (trim the PSU) -> VIH = 3.0-3.15 V. Nothing to set here.
//   B. "repeater pixel": feed ONLY the first LED through 2 series SILICON diodes
//      (1N4148, ~4.1-4.4 V) so it accepts 3.3 V data, and let its DATA OUT — a
//      clean, full VDD-swing regenerated signal — drive the rest of the chain at 5 V.
//      NOT Schottky: this pixel is held dark below, so it draws ~1 mA, and a
//      Schottky drops only ~0.2 V there — too little to buy the margin (see
//      ARENA_LED.md §4 B, corrected 2026-07-31). Measure: repeater VDD 4.0-4.4 V.
// Set this to 1 for option B: pixel 0 is then a hidden repeater, kept dark and
// excluded from the map, so LED numbering in the UI still starts at the first
// visible insert.
// 1 on the Arena bench since 2026-07-31: the hidden repeater is fitted (2 x 1N4148
// off the 5.3 V bus). Measured while it was still being driven lit: 3.70 V, i.e.
// 10 mV BELOW the 0.7 x 5.3 = 3.71 V the rest of the chain needs — it worked, but
// on the wrong side of the spec. Held dark by this switch, its current drops to
// ~1 mA, the diode drop with it, and its VDD should settle near 4.1 V.
#define LED_REPEATER_PIXEL     1

// ---- Front-panel button (optional) -----------------------------------------
// GPIO0 is the BOOT strap and is free once the board is running: short press =
// next lighting mode, long press (>1 s) = night mode toggle. On a DevKitC-1 that
// is the on-board BOOT button; the D1 Mini ESP32 has none, so wire any NO push
// button between D3 (GPIO0) and GND to bring the control out to the frame edge.
// Harmless when nothing is wired: the pin idles high through its pull-up.
// ---- Music mode audio input -------------------------------------------------
// 1 = an electret mic module is wired to GPIO34 (ADC1, WiFi-safe): MAX9814
// (auto-gain, best) or MAX4466 — OUT->GPIO34, VCC->3V3, GND->GND. Leave 0 with
// no mic: a floating ADC pin reads WiFi noise as music and the wall dances to
// static (measured: 290 mA of it). /api/music works either way.
#define ARENA_MIC_ENABLE       0
#define PIN_ARENA_MIC         34

#define ARENA_BUTTON_ENABLE    1
#define PIN_ARENA_BUTTON       0   // active LOW (internal pull-up)
// --- Ecran de controle SSD1306 + encodeur rotatif ---------------------------
// Meme panneau que le compagnon GottFA80+ (Adafruit SSD1306 en I2C) : une seule
// reference pour les deux cartes. Absent = tout le module est inerte, le mur
// n'en depend jamais.
//
// 128x32 suffit : le QR d'appairage Matter ne fait que 21 modules, 29 avec la
// zone de silence obligatoire - mesure, pas estimation (tools/mkqr_header.py).
// En 128x64 il est trace a 2 px par module, bien plus confortable a scanner.
#define ARENA_OLED_ENABLE      1
#define ARENA_OLED_W         128
#define ARENA_OLED_H          32   // 64 si tu montes le grand panneau
#define ARENA_OLED_ADDR     0x3C   // 0x3D sur quelques modules
// L'ecran s'eteint apres ce delai sans action. 0 = ne s'eteint jamais.
//
// ############ PHASE DE TEST - A REMETTRE A 30000 AVANT DE LIVRER ############
// Un OLED qui affiche une image fixe la GRAVE dans son verre. Le QR d'appairage
// laisse en permanence est le pire cas possible : contraste maximal, motif fixe.
// Acceptable quelques heures sur un panneau d'essai, destructeur sur une piece
// vendue et accrochee pour des annees.
#define ARENA_OLED_SLEEP_MS     0

// Ecran affiche au demarrage : 1 = le code d'appairage, 0 = le menu.
// Meme remarque - c'est un reglage de mise au point, pas un defaut de produit.
#define ARENA_OLED_BOOT_QR      1
// ###########################################################################

#if defined(ARENA_BOARD_D1MINI32)
#define PIN_ARENA_OLED_SDA    21
#define PIN_ARENA_OLED_SCL    22
#define PIN_ARENA_ENC_A       32
#define PIN_ARENA_ENC_B       33
#define PIN_ARENA_ENC_SW      25
#else
// Memes broches d'ecran que le GottFA80+ : SDA 47 / SCL 21. GPIO48 est proscrit
// (WS2812 embarquee cablee en dur - du trafic I2C dessus laissait la LED figee
// en blanc sale). L'encodeur prend trois broches libres, a l'ecart de la flash,
// de la PSRAM, des broches de strap et de l'USB.
#define PIN_ARENA_OLED_SDA    47
#define PIN_ARENA_OLED_SCL    21
#define PIN_ARENA_ENC_A        4
#define PIN_ARENA_ENC_B        5
#define PIN_ARENA_ENC_SW       7
#endif


// ---- Soft start -------------------------------------------------------------
// Ramp global brightness 0 -> target over this many ms at boot instead of
// slamming the whole chain on. Limits the inrush into the injection-point bulk
// caps and stops the PSU from hiccup-tripping when 100+ pixels light at once.
#define ARENA_SOFTSTART_MS   900

// Duree maximale du gel du rendu pendant un appairage Bluetooth. Au-dela, on
// repart meme si l'evenement de fermeture BLE n'est jamais venu.
#define ARENA_PAUSE_MAX_MS 120000

// ---- Pixel colour order -----------------------------------------------------
// SK6812MINI-RGBW ships GRBW, which is the default. If reds and greens come out
// swapped in `test` mode, change it live from the web UI (persisted in NVS) —
// no reflash: "grbw", "rgbw", "gbrw", "brgw", "rbgw", "bgrw".
#define ARENA_ORDER_DEFAULT "grbw"

// ---- Power model / safety ---------------------------------------------------
// SK6812MINI-RGBW: 4 dice, ~17.5 mA each at full drive => ~70 mA per LED all-on,
// plus ~1 mA quiescent for the controller. 150 LEDs all-white-all-colours would be
// 150 x 70 mA = 10.5 A, hence the 5 V / 15 A supply (20 % margin) in ARENA_LED.md.
// The firmware never trusts that headroom blindly: every frame is metered and, if
// the estimate exceeds LED_POWER_BUDGET_MA, the whole frame is scaled down before
// it is pushed out. That keeps the PSU, the bus wires and the injection points
// inside their ratings whatever effect is running.
#define LED_MA_PER_CHANNEL    17.5f
#define LED_MA_QUIESCENT       1.0f
#define LED_POWER_BUDGET_MA   9000   // mA ceiling for the whole chain (9 A of a 15 A PSU)
// The budget covers the LEDs ONLY. The controller draws 100-250 mA (peaks higher
// on WiFi transmit) from the same supply and the same fuse, and never appears in
// the estimate. Size a fuse against budget + 300 mA, not against the budget.

// ---- Look & feel defaults ---------------------------------------------------
// Vintage incandescent reference values (see ARENA_LED.md §7).
#define ARENA_AMBER_R 255
#define ARENA_AMBER_G 100
#define ARENA_AMBER_B   0
#define ARENA_AMBER_W   0

#define ARENA_GOLD_R  255
#define ARENA_GOLD_G  140
#define ARENA_GOLD_B    0
#define ARENA_GOLD_W   10

#define ARENA_WARM_R    0
#define ARENA_WARM_G    0
#define ARENA_WARM_B    0
#define ARENA_WARM_W  255

// General illumination, as a fraction of the current colour, under the ROM
// attract mode. A real Arena is NOT dark during attract: the GI bulbs stay lit
// and every insert, driven or not, sits in that glow. Reproducing only the lamp
// matrix leaves the inserts the ROM never touches (L1, L2, L3 among them) looking
// like dead pixels, which is how this was first reported. 0.06 was invisible.
// Expressed as a filament temperature, not a brightness fraction: the GI bulbs
// are incandescent too, so they should sit on the same physical curve as the
// inserts rather than be a dimmed copy of whatever colour is selected.
#define ARENA_GI_T 0.62f
// Boot value of the GI slider, 0..255 (0 = no background at all). Modest by
// default: the background is faithful, but a wall piece that never goes dark is
// a matter of taste and the owner should meet it turned down rather than up.
#define ARENA_GI_DEFAULT 90
// Filament warmth at boot: 0 = spectral (orange), 255 = white-forward. 217 is
// the 0.85 white share the bench settled on before this became a setting.
#define ARENA_WARM_DEFAULT 217

#define ARENA_BRIGHT_DEFAULT 180   // 0..255 global brightness
#define ARENA_SPEED_DEFAULT  128   // 0..255 -> x0.25 .. x4 animation speed
#define ARENA_NIGHT_BRIGHT    96   // braises : voir arena_config.h de l arbre principal

// ---- Filesystem -------------------------------------------------------------
#define ARENA_MAP_PATH "/arena_map.json"   // insert map (editable from the web UI)
