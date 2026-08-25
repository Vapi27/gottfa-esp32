#pragma once
#include <Arduino.h>
#include "arena_config.h"

// ============================================================================
//  Arena Wall-Art LED — render engine.
//
//  Pipeline, once per frame (LED_FRAME_HZ):
//     effect renders into frame[]  (full-scale RGBW, no brightness applied)
//        -> optional crossfade with the previous mode (mode changes never snap)
//        -> identify/zone overlay (mapping wizard)
//        -> global brightness
//        -> power meter + limiter (never exceed LED_POWER_BUDGET_MA)
//        -> push to the SK6812 chain
//
//  Everything is non-blocking: tick() returns immediately when it is not yet
//  time for the next frame, so the web server keeps its latency.
// ============================================================================

namespace arenaled {

struct Rgbw {
  uint8_t r, g, b, w;
};

enum Mode : uint8_t {
  MODE_OFF = 0,     // all pixels dark (the chain stays powered/refreshed)
  MODE_CLASSIC,     // static warm white with a subtle incandescent flicker
  MODE_ATTRACT,     // slow pulses / waves / chases / random inserts, auto-cycled
  MODE_ARENA,       // animations that follow the playfield layout (zone-driven)
  MODE_NIGHT,       // warm white at ~10 %
  MODE_RAINBOW,     // full RGB hue sweep
  MODE_MUSIC,       // audio-reactive: mic on GPIO34, or /api/music pushes
  MODE_TEST,        // R / G / B / W channel walk — wiring + colour-order check
  MODE_COUNT
};

void begin();
void tick();                          // call from loop()

void        setMode(Mode m);
Mode        mode();
// Au retour du courant : rallumer le dernier mode utilise (true, defaut) ou
// restaurer exactement l'etat d'avant, extinction comprise (false).
bool        bootOn();
void        setBootOn(bool b);
const char* modeName(Mode m);   // identifiant : NVS + API
const char* modeLabel(Mode m);  // libelle client, libre de changer
Mode        modeFromName(const char* s);   // MODE_COUNT if unknown
void        nextMode();                    // front-panel button: cycle usable modes

void    setBrightness(uint8_t b);
uint8_t brightness();

// General illumination under the ROM attract mode, 0..255 (0 = off). A real
// Arena's GI bulbs stay lit through attract, so an insert the ROM never drives
// still glows — but whether a wall piece should carry that permanent background
// is taste, not fidelity, so it is a live setting rather than a constant.
void    setGi(uint8_t g);
uint8_t gi();
void    setDensity(uint8_t d);
uint8_t density();

// Filament warmth, 0..255. 0 is the pure spectral split — colorimetrically
// right and, on a wall, orange. 255 hands the output to the white die as soon
// as the filament is hot. Both end points are the same black-body curve; this
// only decides which die carries it.
void    setWarm(uint8_t w);
uint8_t warm();

// Lamps held lit through the ROM attract, as a PinMAME-lamp bit mask.
// A machine that has been played does not go back to a virgin attract: some
// lamps stay latched from the last game, and a cold-boot capture cannot contain
// them. This is that latch, made explicit and editable, rather than a doctored
// sequence.
void     setLatched(uint64_t m);
uint64_t latched();

// Incandescent simulation on/off. Off is a plain switch: full level, no thermal
// lag, no colour shift — which is what you want when the inserts carry their own
// colours and you would rather see them cleanly than through a filament.
// Pause the render loop entirely - no frames, no pixel pushes. Used while a
// phone is connected over BLE for Matter pairing: the PASE crypto wants the
// CPU and RAM, and the RMT traffic wants neither competitor nor spectator.
void setPaused(bool p);
bool paused();

void setIncandescent(bool on);
bool incandescent();

// Feed the music mode from outside (a phone app, a PC script, anything that can
// hit REST). Values 0..255; an external push silences the on-board mic for 2 s.
void musicPush(uint8_t energy, uint8_t bass, uint8_t treble);
void    setSpeed(uint8_t s);               // 0..255 -> x0.25 .. x4
uint8_t speed();
void    setColor(Rgbw c);                  // base colour for CLASSIC / ATTRACT / ARENA
Rgbw    color();
void    setCount(uint16_t n);              // live LED count (1..LED_MAX)
uint16_t count();
void    setBudgetMa(uint16_t ma);          // power ceiling for the whole chain
uint16_t budgetMa();

// Deux faits de CABLAGE, donc reglables sans reflasher : la chaine porte-t-elle
// un pixel repeteur en tete (tenu eteint), et sur quelle broche sort la data.
// Une carte controleur du commerce n'a ni l'un ni l'autre des valeurs du banc.
bool     repeater();
void     setRepeater(bool on);
uint8_t  pin();
void     setPin(uint8_t p);

// Nombre de murs qui se PARTAGENT la meme alimentation (1 = seul). Le plafond
// effectif devient budgetMa() / share : quatre murs chaines sur un chargeur de
// 3 A ne peuvent pas en reclamer 8 chacun de leur cote. Pose par arena_peers
// quand le proprietaire declare ses murs chaines ; ne touche PAS a la valeur
// enregistree, qui reste celle du budget d'un mur seul.
void    setBudgetShare(uint8_t n);
uint8_t budgetShare();

// Pixel colour order, e.g. "grbw" (SK6812 default) / "rgbw" / "gbrw" / ...
// Changing it re-types the chain live — no reflash, no reboot.
bool        setOrder(const char* s);       // false if the string is not a known order
const char* order();

// --- mapping helpers (web UI wizard) ---------------------------------------
void identifyLed(int led, uint32_t ms);    // spotlight one pixel, any mode, then auto-clear
void identifyZone(int zoneIdx, uint32_t ms);
void clearIdentify();
int  identifyingLed();                     // -1 when idle

// --- telemetry --------------------------------------------------------------
// Defaut signale par le limiteur de sortie U5. Vrai seulement si le signal a
// persiste : voir ARENA_FAULT_HOLD_MS. Le compteur, lui, garde la trace des
// episodes meme brefs une fois passe le demarrage.
bool     ledFault();
uint16_t ledFaultCount();

float    lastAmps();       // A estimated for the frame just pushed
bool     limited();        // true if the power limiter had to scale the last frame
uint32_t frameCount();
uint16_t fps();

void save();               // persist mode/brightness/speed/colour/count to NVS

// Deux remises a zero, et la distinction n'est pas cosmetique. resetLook() rend
// l'APPARENCE a ses valeurs d'usine - mode, luminosite, vitesse, halo, blanc
// chaud, densite, filament, couleur - et laisse intacts le nombre de pixels,
// leur ordre, le budget de courant et la cartographie : ceux-la decrivent
// comment CE mur est construit, et les effacer transforme un mur qui marche en
// mur mal pilote. resetAll() efface tout, y compris ces faits materiels, et ne
// doit servir qu'a preparer une carte pour quelqu'un d'autre.
void resetLook();
void resetAll();

}  // namespace arenaled
