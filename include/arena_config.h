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
// ⚠️ Le point d'acces de secours ne porte PAS de nom fixe : arena_net.cpp diffuse
// s_name, qui vaut par defaut "Playfield-<2 derniers octets de la MAC>". C'est
// voulu - deux murs dans la meme maison ne se marchent pas dessus, et l'adresse
// mDNS en decoule. Un #define ARENA_AP_SSID "Arena-LED" trainait ici sans etre
// utilise nulle part, et les deux notices client l'ont recopie : le client
// cherchait au deballage un reseau qui n'existe pas. Supprime plutot que corrige,
// pour qu'il ne puisse plus induire personne en erreur.
#define ARENA_AP_PASS        "pinball87"    // >= 8 chars

// Puissance d emission WiFi, en quarts de dBm (esp_wifi_set_max_tx_power) :
// 80 = 20 dBm, le maximum de la radio, et le plus gros appel de courant du
// firmware. Une rafale d emission tire ~350 mA pendant quelques centaines de
// microsecondes ; sur une alimentation juste, c est elle qui fait plonger le
// 3,3 V et declenche le detecteur de brownout. La valeur SAFE n est PAS le
// defaut : elle ne s applique que si le reset precedent etait justement un
// brownout, pour que la carte revienne plus douce d elle-meme au lieu de
// boucler sur le meme effondrement. 52 = 13 dBm, soit ~5x moins de puissance.
#define ARENA_WIFI_TXPWR_QDBM       80
#define ARENA_WIFI_TXPWR_SAFE_QDBM  52

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
// Micro embarque. Le mode Music retombe sur musicPush() (energie envoyee par
// l'API) quand il vaut 0 - c'est ce qui masquait le fait que la broche etait
// invalide : le chemin micro n'etait meme pas compile.
#define ARENA_MIC_ENABLE       1
// Micro. DOIT etre sur ADC1 (GPIO1-10) : ADC2 est utilise par le pilote WiFi sur
// l'ESP32-S3, et une lecture y echoue des que la radio emet. GPIO34 - la valeur
// precedente - n'est ni l'un ni l'autre, et appartient a la PSRAM octale sur un
// module N16R8 : le mode Music lisait donc du vide.
#define PIN_ARENA_MIC          1   // ADC1_CH0

#define ARENA_BUTTON_ENABLE    1
// Bouton de facade. PAS sur GPIO0 : c'est la broche de selection du mode de
// demarrage du S3, et un client qui l'appuie en branchant l'alimentation obtient
// une carte partie en mode televersement - un mur qui parait mort. GPIO0 reste
// pour un poussoir BOOT cote carte, hors de portee.
// Bouton de facade : ABANDONNE sur la carte de serie (trois poussoirs lateraux
// suffisent, le menu couvre tout). A 0, GPIO18 est reellement libre.
#define ARENA_FACE_BTN_ENABLE  0
#define PIN_ARENA_BUTTON      18   // actif a l'etat bas (tirage interne)
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
#define ARENA_OLED_H          32   // 64 pour le 0,96" carre
#define ARENA_OLED_ADDR     0x3C   // 0x3D sur quelques modules
// L'ecran s'eteint apres ce delai sans action, et le panneau est coupe, pas
// seulement efface. Un OLED qui affiche une image fixe la GRAVE dans son verre,
// et cette piece reste accrochee au mur pendant des annees.
// 0 = ne s'eteint jamais : reglage de mise au point uniquement.
#define ARENA_OLED_SLEEP_MS 30000

// Ecran affiche au demarrage : 1 = le code d'appairage, 0 = le menu.
#define ARENA_OLED_BOOT_QR      0

#if defined(ARENA_BOARD_D1MINI32)
#define PIN_ARENA_OLED_SDA    21
#define PIN_ARENA_OLED_SCL    22
#define PIN_ARENA_ENC_A       32
#define PIN_ARENA_ENC_B       33
#define PIN_ARENA_ENC_SW      25
#define PIN_ARENA_BTN_UP      18
#define PIN_ARENA_BTN_DOWN    19
#define PIN_ARENA_BTN_OK      PIN_ARENA_ENC_SW
// Retour de defaut du limiteur (voir la branche S3 pour le detail electrique).
// GPIO4 est libre sur le D1 Mini (D2), hors flash, hors strap, hors UART, et il
// a un tirage interne - ce que la lecture a drain ouvert exige.
#define PIN_ARENA_LED_FAULT    4
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

// Encodeur rotatif : ABANDONNE sur la carte de serie. Trois poussoirs lateraux
// en bord de carte font le meme travail sans piece traversante ni bouton qui
// depasse au dos. A 0, GPIO4 et GPIO5 sont LIBRES et aucune interruption n'est
// accrochee a des broches non connectees.
// Navigation a trois boutons : deux fleches et OK. C'est le montage retenu pour
// la carte definitive - trois poussoirs CMS coutent moins qu'un encodeur, se
// posent a plat derriere la face avant, et se serigraphient sans ambiguite.
//
// Les deux entrees coexistent : l'encodeur reste lu s'il est cable, et OK
// partage volontairement la broche de son bouton-poussoir. Une carte n'a donc
// jamais besoin des deux, et le firmware est le meme dans les deux cas.
// GPIO6 serait le choix evident et il est DEJA PRIS : c'est PIN_LED_DATA2, la
// seconde chaine de pixels, reellement instanciee dans arenaled.cpp. Un poussoir
// en pull-up interne s'y opposerait a une sortie RMT.
// 15 et 17 sont libres sur le WROOM-1 : hors flash (26-32), hors PSRAM octale
// (33-37), hors USB (19/20) et hors broches de strap.
// Brochage des trois poussoirs : la source de verite est le CUIVRE, pas un
// symptome. hardware/NETLIST.md, table des liaisons :
//   BTN_LEFT  U1 br.8  (IO15) -> S1 -> GND
//   BTN_RIGHT U1 br.10 (IO17) -> S2 -> GND
//   BTN_OK    U1 br.7  (IO7)  -> S3 -> GND
// et hardware/PCB_HARDWARE.md : "3 poussoirs tactiles CMS : haut GPIO15, bas
// GPIO17, OK GPIO7".
//
// Ces trois lignes ont ete tournees d'un cran le 2026-08-25 sur la foi d'une
// description de symptomes ("droite fait gauche, OK fait droite"). C'etait une
// erreur : le vrai defaut etait une soustraction non signee dans le test de
// veille de l'ecran, qui endormait le panneau a chaque appui et faisait passer
// des boutons parfaitement cables pour des boutons decales. Le netlist existait
// et repondait a la question ; il n'a pas ete lu. Remis conforme au cuivre.
#define PIN_ARENA_BTN_UP      15                  // S1, poussoir GAUCHE / haut
#define PIN_ARENA_BTN_DOWN    17                  // S2, poussoir DROITE / bas
#define PIN_ARENA_BTN_OK       PIN_ARENA_ENC_SW   // S3, GPIO7

// Retour de defaut du limiteur de sortie U5 (AP22652, broche 4 ~FAULT).
// Collecteur ouvert, actif BAS. Tire au haut par le tirage INTERNE de l'ESP :
// pas de resistance externe, le signal est lent et le tirage interne suffit.
// !! Ne JAMAIS le tirer vers le 5 V : U5 est alimente en 5 V, mais sa sortie
// est a drain ouvert, donc c'est le tirage qui fixe le niveau haut - a 5 V on
// mettrait 5 V sur une broche de l'ESP.
// GPIO4 est libre depuis l'abandon de l'encodeur.
#define PIN_ARENA_LED_FAULT    4

// Le limiteur passe en limitation a CHAQUE mise sous tension, le temps de
// charger la capacite de la chaine : ~FAULT s'active sans qu'il y ait de
// probleme. On l'ignore donc pendant le demarrage, puis on exige qu'il persiste
// - un defaut fugitif est du bruit, un court-circuit dure.
#endif

// Reglages communs a toutes les cibles. Ils vivaient dans la branche #else
// (S3) : arenaled.cpp et arena_oled.cpp les utilisent SANS condition, donc
// env:arenaled_d1mini32 ne compilait plus du tout - et c'est la cible que
// tools/arena_flash.sh choisit par defaut.
//
// Encodeur rotatif : ABANDONNE sur la carte de serie (trois poussoirs lateraux
// font le meme travail). A 0, ses broches restent libres et aucune interruption
// n'est accrochee a des entrees non connectees.
#define ARENA_ENC_ENABLE       0

// Le limiteur passe en limitation a CHAQUE mise sous tension, le temps de
// charger la capacite de la chaine : ~FAULT s'active sans qu'il y ait de
// probleme. On l'ignore donc pendant le demarrage, puis on exige qu'il persiste
// - un defaut fugitif est du bruit, un court-circuit dure.
#define ARENA_FAULT_IGNORE_MS 1500     // apres le boot (le soft-start dure 900 ms)
#define ARENA_FAULT_HOLD_MS    200     // duree minimale pour declarer un defaut


// ---- Pixel de temoin sur la carte -------------------------------------------
// Beaucoup de cartes controleur portent un pixel adressable de statut. C'est le
// SEUL signe de vie quand aucun ruban n'est branche : sans lui, une carte qui
// tourne parfaitement et une carte morte se ressemblent trait pour trait. La
// broche varie d'une carte a l'autre - 48 sur un DevKitC-1, autre chose sur une
// carte du commerce - donc elle est reglable a chaud (/api/set?statuspin=N).
#define ARENA_STATUS_LED_ENABLE  1
#if defined(ARENA_BOARD_D1MINI32)
#define ARENA_STATUS_PIN         2    // LED simple sur le D1 Mini, pas un pixel
#else
#define ARENA_STATUS_PIN        48    // STATUS_PX : U1 br.25 (IO48) -> D2 (NETLIST.md)
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
// Courant des canaux, LU DANS LE DATASHEET SK6812MINI-RGBW (Normand, rev. 01,
// section 11, conditions d'essai) :
//
//     Iout R/G/B = 9 mA        par canal couleur
//     Iout W     = 18 mA       le blanc dedie, le DOUBLE
//     IDD        = 1 mA        consommation statique du controleur
//
// Le modele precedent multipliait les quatre canaux par une constante unique,
// ce qui surestime les couleurs d'un facteur 2 et fausse le pire cas. Il valait
// 17,5 mA - une moyenne sans source - puis 25 mA cale sur une seule mesure au
// wattmetre, avant que le datasheet ne donne les vrais chiffres.
//
// Recoupement a trois voies, le 2026-08-07 : en mode `classic` seul le canal W
// est pilote, donc le datasheet predit 40 x 18 + 40 x 1 = 0,76 A pour 40
// pixels. La mesure a la prise donne 5,25 W de LED, soit 0,76 A continu pour un
// rendement de bloc de 72 % - plausible pour un petit bloc a 15 % de charge.
// Datasheet et wattmetre concordent.
//
// Pire cas reel, 150 pixels tous canaux a fond : 150 x (9+9+9+18+1) = 6,9 A.
#define LED_MA_RGB             9.0f
#define LED_MA_W              18.0f
#define LED_MA_QUIESCENT       1.0f
// Plafond de courant de TOUTE la chaine. Le firmware assombrit la trame entiere
// plutot que de le depasser - c'est donc lui, et non le cuivre, qui garantit que
// la carte ne tire jamais plus que son alimentation n'accepte.
//
// Ce plafond n'est pas un chiffre rond choisi au hasard : il est FIXE PAR LE
// MATERIEL, par le seuil le plus bas que l'interrupteur a limitation puisse
// prendre. U5 = AP22652 avec RLIM = 11 k : 2174 / 2416 / 2657 mA garantis sur
// -40 a +85 degres. Le plafond doit rester franchement SOUS 2174 mA, sans quoi
// la protection se declenche sur une trame blanche legitime et le mur affiche
// "DEFAUT SORTIE" alors que rien n'est casse. 1900 mA laisse 14 %.
//
// CORRIGE le 2026-08-09 : c'etait 2000 mA, cale sur l'AP2552 (2200 mA au pire
// bas). L'AP22652 qui le remplace limite plus haut a resistance egale, R4 est
// donc passee de 10 a 11 k et la fenetre s'est deplacee. Les deux se suivent :
// toucher a R4 sans toucher a ce plafond casse la marge.
//
// Autre borne, independante : l'AP22652 n'est donne que pour 2,1 A CONTINU
// (-40..+85). 1900 mA, c'est 90 % de sa capacite, la ou 2000 en faisaient 95 %.
//
// A 25,9 mA par pixel MESURES (regression sur cinq points, 2026-08-07), 1,9 A
// couvre 73 pixels a fond. Le mode attract n'en demande que 0,29 A : ce plafond
// ne mord que sur du blanc plein synthetique.
#define LED_POWER_BUDGET_MA   1900
// Plafond DUR, impose par U5 : au-dela, c'est le limiteur qui decide et le mur
// se declare en defaut. Aucun reglage, aucune requete HTTP ne doit passer outre.
//
// ABAISSE DE 2100 A 1900 LE 2026-08-10. 2100 etait une marge fictive : avec
// R4 = 11 kOhm +/-1 %, les equations best-fit du datasheet AP22652 donnent un
// ILIMIT *minimum* de 2,10 a 2,14 A. Un plafond a 2100 mA laissait donc 0,2 a
// 2 % de marge sur le point de declenchement le plus defavorable du limiteur :
// a pleine charge legitime, en bout de tolerance, U5 se declenchait. A 1900 la
// marge remonte a ~244 mA. Correctif a cout materiel nul -- l'alternative etait
// de rechanger R4, deja passee de 10 k a 11 k lors du remplacement AP2552.
// ---- Le limiteur U5, et le plafond qui en decoule ---------------------------
//
// UN SEUL endroit decrit le materiel. Le plafond n'est plus un nombre ecrit a la
// main : il se calcule. Passer la carte a un courant plus eleve, c'est changer
// les quatre lignes ci-dessous et rien d'autre - le reste du firmware suit, et
// le controle de coherence en fin de bloc refuse une combinaison impossible.
//
// Les deux bornes sont INDEPENDANTES et il faut retenir la plus basse :
//   - ce que la PIECE supporte en continu (rating de la fiche) ;
//   - ou se declenche le LIMITEUR au pire cas, fixe par R4.
#define ARENA_LIMITER_NAME        "AP22652"
#define ARENA_LIMITER_RLIM_KOHM   11      // R4, 1 %
#define ARENA_LIMITER_CONT_MA     2100    // courant continu garanti, -40..+85 (DS41186 Rev 5-2)
#define ARENA_LIMITER_TRIP_MIN_MA 2174    // ILIMIT le plus defavorable a R4 = 11 k
// Marge sous la borne retenue. 200 mA reproduit exactement le plafond de 1900 mA
// etabli le 2026-08-10, apres qu'un plafond a 2100 se soit revele fictif : il ne
// laissait que 0,2 a 2 % sous le point de declenchement le plus defavorable, et
// U5 partait en defaut a pleine charge legitime.
#define ARENA_LIMITER_MARGIN_MA   200

#define ARENA_LIMITER_FLOOR_MA \
  (ARENA_LIMITER_TRIP_MIN_MA < ARENA_LIMITER_CONT_MA \
     ? ARENA_LIMITER_TRIP_MIN_MA : ARENA_LIMITER_CONT_MA)

#define LED_POWER_BUDGET_MAX  (ARENA_LIMITER_FLOOR_MA - ARENA_LIMITER_MARGIN_MA)

// ---- Viser 3 A : ce qu'il faut changer, et ce que ca entraine ---------------
//
// ⚠️ Ce n'est PAS un changement logiciel. Monter ce plafond seul ne donne pas
// 3 A : au-dela de sa fenetre, ce n'est plus le firmware qui assombrit la trame,
// c'est U5 qui ecrete et leve son drapeau de defaut. Le mur clignoterait et se
// declarerait en panne, sans que rien dans l'interface n'explique pourquoi.
//
// Quatre choses doivent bouger ensemble :
//
//  1. LA PIECE. L'AP22652 est donne pour 2,1 A CONTINU. Aucune valeur de R4 ne
//     le fait tenir 3 A : R4 deplace le seuil de declenchement, pas le rating
//     thermique. Il faut un limiteur donne pour >= 3,5 A continu.
//  2. R4. La fiche AP22652 donne ILIMIT_typ[mA] = 30321 / R[kOhm]^1,055 avec une
//     fenetre de +/-10 %. Pour un declenchement TYPIQUE a 3,3 A il faudrait
//     R ~ 8,1 k, dont le minimum tomberait vers 3,0 A - soit zero marge a 3 A.
//     Toute piece de remplacement a sa propre equation : la relire, ne pas
//     reutiliser celle-ci.
//  3. LE CUIVRE. Mesure consignee dans hardware/PCB_HARDWARE.md : 0,24 mm de
//     large a 2,1 A donnent deja 0,43 V de chute et 0,9 W dissipes. A 3 A la
//     meme piste perdrait 0,61 V et dissiperait 1,9 W. Il faut l'elargir.
//  4. L'ALIMENTATION ET LE FUSIBLE. Le budget ne couvre que les LED ; le
//     controleur tire 100-250 mA de la meme source. Un budget de 3 A demande une
//     alimentation 5 V / 4 A et un fusible dimensionne sur 3,3 A.
//
// Une fois le materiel change, il suffit de mettre a jour les quatre defines
// ci-dessus : le plafond, l'ecretage de setBudgetMa() et la borne relue en NVS
// suivent tout seuls. Exemple pour une piece 3,5 A continu avec un R4 donnant
// 3,4 A au pire cas :
//     #define ARENA_LIMITER_CONT_MA     3500
//     #define ARENA_LIMITER_TRIP_MIN_MA 3400
//     -> plafond = 3200 mA, donc 3 A utilisables avec 200 mA de marge.
_Static_assert(LED_POWER_BUDGET_MAX > 0,
               "plafond negatif : la marge depasse ce que le limiteur autorise");
_Static_assert(LED_POWER_BUDGET_MA <= LED_POWER_BUDGET_MAX,
               "le budget par defaut depasse ce que le limiteur autorise");
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
// Position NEUTRE du gain des champignons : a cette valeur, le groupe est rendu
// exactement comme le reste du mur. Au-dessus il est pousse, en dessous il est
// retenu. 128 place le neutre au milieu du curseur, et laisse un facteur deux de
// chaque cote - assez pour rattraper un capuchon epais sans transformer le
// reglage en interrupteur.
// Duree du maintien des TROIS boutons pour une remise a zero d'usine sans ecran.
// Cinq secondes : assez long pour qu'on ne le fasse pas par megarde, assez court
// pour qu'un client au telephone puisse le tenir en etant guide.
#define ARENA_BLIND_RESET_MS 5000

#define ARENA_CHAMP_NEUTRAL 128

#define ARENA_GI_T 0.62f
// Boot value of the GI slider, 0..255 (0 = no background at all). Modest by
// default: the background is faithful, but a wall piece that never goes dark is
// a matter of taste and the owner should meet it turned down rather than up.
#define ARENA_GI_DEFAULT 90

// Nom du groupe traite comme "les champignons" : un troisieme etage permanent,
// a cote du fond, avec son propre niveau. C'est un groupe ordinaire de arenamap
// - on lui affecte des pixels avec l'outil de groupes habituel - et non une
// notion cablee dans le rendu. S'il n'existe pas, le reglage n'a simplement
// aucun membre et ne fait rien.
#define ARENA_CHAMP_ZONE "champignons"
// Filament warmth at boot: 0 = spectral (orange), 255 = white-forward. 217 is
// the 0.85 white share the bench settled on before this became a setting.
#define ARENA_WARM_DEFAULT 217

#define ARENA_BRIGHT_DEFAULT 180   // 0..255 global brightness
#define ARENA_SPEED_DEFAULT  128   // 0..255 -> x0.25 .. x4 animation speed
#define ARENA_NIGHT_BRIGHT    96   // braises : voir arena_config.h de l arbre principal

// ---- Filesystem -------------------------------------------------------------
#define ARENA_MAP_PATH "/arena_map.json"   // insert map (editable from the web UI)
