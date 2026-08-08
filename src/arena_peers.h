// arena_peers.h — les murs se voient et se repondent.
//
// Un mur emet une balise UDP en diffusion toutes les deux secondes et ecoute
// celles des autres. Il en tire deux choses : la liste des murs presents (pour
// l'afficher, et pour savoir lequel on met a jour), et, si le proprietaire le
// demande, la synchronisation de leur etat.
//
// Trois comportements, dans l'ordre d'engagement :
//
//   OFF     chacun pour soi. Les voisins sont quand meme detectes et listes -
//           c'est utile en soi, et ca ne touche a rien.
//   MIRROR  ce qu'on change sur un mur se produit sur tous, tout de suite.
//   RELAY   pareil, mais decale : chaque mur attend son rang, et le changement
//           balaie la piece de gauche a droite. C'est le mode qui rend quatre
//           murs interessants plutot que redondants.
//
// Le piege de ce genre de dispositif est la boucle : A adopte l'etat de B, le
// re-annonce, B l'adopte a son tour, et les deux s'excitent sans fin. On
// l'evite avec une horloge logique (Lamport) portee par la balise : un etat
// n'est adopte que s'il est STRICTEMENT plus recent, les egalites etant
// tranchees par la MAC. Un etat deja adopte n'est donc jamais re-adopte.
#pragma once
#include <Arduino.h>

namespace arenapeers {

enum Link : uint8_t { LINK_OFF = 0, LINK_MIRROR = 1, LINK_RELAY = 2 };

void begin();
void tick();                    // depuis loop(); ne fait rien sans adresse IP

uint8_t     count();            // murs voisins vivants (soi non compris)
String      json();             // tableau JSON des voisins, pour /api/state

Link        link();
void        setLink(Link l);
const char* linkName(Link l);
Link        linkFromName(const char* s);

// Rang de ce mur dans l'ordre des MAC, voisins compris. C'est lui qui donne au
// mode RELAY son sens de balayage, et il est stable : la MAC ne bouge pas.
uint8_t rank();

// Efface le reglage de liaison memorise. Le mur repart independant, ce qui est
// le seul etat sur quand on ne sait pas chez qui il va atterrir.
void resetAll();

// Les murs partagent-ils la meme alimentation (chainage USB-C) ? Quand c'est
// vrai, chaque mur divise son plafond de courant par le nombre de murs vus, ce
// qui empeche une chaine de reclamer plus que le chargeur ne donne.
bool sharedPower();
void setSharedPower(bool on);

}  // namespace arenapeers
