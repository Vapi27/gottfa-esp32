#pragma once
#include <Arduino.h>

// WiFi (STA with SoftAP fallback) + mDNS + web UI / REST for the Arena LED wall.
namespace arenanet {

void begin();
#ifdef ARENA_MATTER
void matterTick();
#endif
void loop();
const char* ip();

// Nom de CE mur. Sert des qu'il y en a plus d'un : 4 murs sur le reseau, ce
// sont sinon 4 adresses IP anonymes et 4 accessoires identiques dans Maison.
// Par defaut derive de la MAC (unique d'usine), remplacable par /api/name.
const String& wallName();
const char* mode();          // "STA" / "SoftAP"

}  // namespace arenanet
