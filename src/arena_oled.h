// arena_oled.h — optional SSD1306 status/control screen for the wall.
//
// Same panel family as the GottFA80+ companion (Adafruit SSD1306 over I2C), so
// one part number covers both boards. Absent panel = every entry point is a
// no-op; the wall never depends on it.
//
// It is a CONTROL surface, not just a status one: mode, brightness, speed,
// filament, and the Matter pairing code, reachable without a phone. That last
// one matters more than it looks — the pairing code is the one thing an owner
// needs exactly when the wall is not yet on the network, which is precisely
// when the web UI cannot help them.
//
// The screen blanks after ARENA_OLED_SLEEP_MS without input and the panel is
// powered down, not merely cleared: an OLED left showing a static menu burns
// that menu into the glass, and this thing hangs on a wall for years.
#pragma once
#include <Arduino.h>

namespace arenaoled {

void begin();          // I2C + panel probe. Safe to call when no panel is fitted.
void tick();           // call from loop(); cheap when asleep
bool found();          // true if a panel answered — lets the caller skip its own UI

// Any human input, wherever it came from (encoder, button, web page). Wakes the
// screen and restarts the sleep countdown. Calling it while awake is harmless.
void poke();

// Affiche le code d'appairage en plein ecran, sans passer par l'encodeur.
// C'est LA chose qu'un proprietaire doit pouvoir obtenir quand le mur n'est pas
// encore sur le reseau - et pendant les essais, quand le bouton n'est pas cable.
void showQr();

// Etat BRUT des trois entrees, sans anti-rebond ni interpretation, plus le
// nombre de declenchements depuis le demarrage. Un poussoir colle ou une broche
// qui n'est pas un poussoir se voit ici et nulle part ailleurs : au repos les
// trois doivent lire "haut".
void btnRaw(bool& up, bool& okd, bool& down,
            uint32_t& nUp, uint32_t& nOk, uint32_t& nDown);

}  // namespace arenaoled
