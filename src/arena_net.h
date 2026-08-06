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

// Efface le nom du mur et les identifiants WiFi memorises. Sans effet sur
// l'appairage Matter, qui vit dans son propre magasin.
void resetNetwork();
// Quitte toutes les maisons Matter. Emporte le WiFi avec lui dans la version
// Matter : c'est l'appairage qui le fournit, il n'est stocke nulle part ici.
void forgetHomes();
const char* mode();          // "STA" / "SoftAP"

}  // namespace arenanet
