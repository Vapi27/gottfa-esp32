// chefv2.h — CHEF v2 : conducteur EVENEMENTIEL. Le CPU de la ROM tourne (conducteur, sans synthese)
// et ses evenements (ecritures AY / flux DAC / trames SP0250) pilotent le lecteur WAV — par PROPRIETE
// de canal, sans aucun seuil devine : une ecriture volume=0 est un stop exact ; un canal revendique
// par un autre son est un vol ; la fin du flux DAC est la fin du sample.
// UNE seule implementation : compilee sur Mac (simulateur/validation) ET dans le firmware.
// Les signatures par son (canaux, usage dac/sp, pause interne max) viennent de l'analyse hors-ligne
// (tools/host_sig2 -> sounds.sig), generees par jeu, sans intervention.
// (C) 2026 Valere Pilpil / Pstore.
#pragma once
#include <stdint.h>
#include "psorom.h"

namespace chefv2 {

struct Sig {                      // signature d'un son (extraite de la ROM, hors-ligne)
  uint8_t  id;                    // commande bus (0-95 avec banques)
  uint8_t  ay0, ay1;              // masques canaux AY revendiques (bits 0-2)
  uint8_t  dac, sp;               // utilise le DAC / le SP0250
  uint8_t  ym;                    // masque voix YM2151 revendiquees (Gen3, 8 voix)
  uint8_t  sustained;             // la ROM l'entretient (boucle) tant qu'on ne l'arrete pas
  uint32_t durMs;                 // duree naturelle (one-shot)
  uint32_t gapMs;                 // plus longue pause interne mesuree -> confirmation de stop par-son
  uint32_t onMs;                  // latence commande -> premiere activite mesuree -> grace de demarrage
};

struct Action { uint32_t tMs; uint8_t op; uint8_t id; };   // op : 0=START 1=STOP 2=RESTART

void begin(const Sig* sigs, int n, uint32_t clkPerMs);     // clkPerMs : unites wallclk par ms (Gen1=1000)
void reset();
void command(uint8_t id, uint32_t clk);                    // commande recue (a envoyer AUSSI a la ROM par l'appelant)
void feed(const psorom::Ev* ev, int n);                    // evenements du conducteur
void tick(uint32_t clk);                                   // horloge emulee courante (verifie les fins de son)
int  drain(Action* out, int maxN);                         // actions WAV a executer
uint64_t aliveMask();                                      // bit n = le son n est actif (suivi/GC)
void clearKills();                                         // vide la table d'interactions (avant chargement d'un jeu)
void setKill(uint8_t victim, uint8_t thief);               // paire MESUREE (pair_scan) : thief REMPLACE victim
void setKeep(uint8_t victim, uint8_t thief, uint16_t ttlMs);   // paire MESUREE : victim SURVIT a thief... ttlMs (sa vraie duree dessous)

} // namespace chefv2
