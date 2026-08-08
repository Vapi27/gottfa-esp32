#include "arena_peers.h"
#include "arena_net.h"
#include "arenaled.h"
#include "arena_config.h"

#include <Preferences.h>
#include <lwip/sockets.h>
#include <string.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <esp_mac.h>
#else
#include <esp_system.h>
#endif

namespace arenapeers {

// ---------------------------------------------------------------------------
//  Protocole
// ---------------------------------------------------------------------------
// Un port fixe et une structure fixe : pas de JSON sur le fil. Une balise est
// emise toutes les deux secondes, et en rafale de trois quand l'etat local
// change - une diffusion UDP se perd sans prevenir, et un mur qui rate LE
// paquet du changement resterait desynchronise jusqu'a la balise suivante.
static const uint16_t PORT        = 41827;
static const uint32_t BEACON_MS   = 2000;
static const uint32_t BURST_MS    = 180;    // entre les paquets d'une rafale
static const uint8_t  BURST_N     = 3;
static const uint32_t PEER_TTL_MS = 8000;   // quatre balises manquees = disparu
static const uint8_t  MAX_PEERS   = 7;
static const uint32_t RELAY_STEP_MS = 250;  // decalage par rang en mode RELAY

struct __attribute__((packed)) Beacon {
  char     magic[4];      // "PFW1"
  uint8_t  ver;           // 1
  uint8_t  link;          // Link de l'emetteur
  uint8_t  mode;          // index de mode arenaled
  uint8_t  bright;
  uint8_t  mac[6];        // qui emet
  uint8_t  omac[6];       // qui est a l'origine de l'etat porte ici
  uint32_t oseq;          // horloge logique de cette origine
  char     name[24];      // nom lisible, pour l'affichage
};
static_assert(sizeof(Beacon) == 48, "la balise doit rester compacte et stable");

struct Peer {
  uint8_t  mac[6];
  char     name[24];
  uint32_t ip;
  uint8_t  mode;
  uint8_t  bright;
  uint8_t  link;
  uint32_t seen;
};

// ---------------------------------------------------------------------------
//  Etat
// ---------------------------------------------------------------------------
static int         s_sock = -1;
static Preferences s_prefs;
static Link        s_link = LINK_OFF;
static bool        s_sharedPwr = false;
static uint8_t     s_mac[6] = {0};

static Peer     s_peers[MAX_PEERS];
static uint8_t  s_nPeers = 0;

// Horloge logique de l'etat qu'on porte, et son auteur.
static uint32_t s_oseq = 0;
static uint8_t  s_omac[6] = {0};

// Dernier etat local connu. On le compare a chaque tour plutot que d'accrocher
// un rappel sur chaque point d'entree : un changement peut venir du web, de
// Siri, du bouton ou de l'encodeur, et cette comparaison les attrape tous sans
// qu'aucun n'ait a se souvenir de nous.
static uint8_t  s_lastMode = 255;
static uint8_t  s_lastBright = 255;

static uint32_t s_nextBeacon = 0;
static uint8_t  s_burst = 0;
static uint32_t s_nextBurst = 0;

// Adoption differee, pour le mode RELAY.
static bool     s_pending = false;
static uint32_t s_pendingAt = 0;
static uint8_t  s_pendMode = 0, s_pendBright = 0;

// ---------------------------------------------------------------------------
//  Reglage
// ---------------------------------------------------------------------------
const char* linkName(Link l) {
  switch (l) {
    case LINK_MIRROR: return "mirror";
    case LINK_RELAY:  return "relay";
    default:          return "off";
  }
}
Link linkFromName(const char* s) {
  if (!s) return LINK_OFF;
  if (!strcmp(s, "mirror")) return LINK_MIRROR;
  if (!strcmp(s, "relay"))  return LINK_RELAY;
  return LINK_OFF;
}
Link link() { return s_link; }

void setLink(Link l) {
  if (l == s_link) return;
  s_link = l;
  s_prefs.putUChar("link", (uint8_t)l);
  s_burst = BURST_N;                 // que les voisins l'apprennent tout de suite
  s_nextBurst = 0;
}

uint8_t count() { return s_nPeers; }

bool sharedPower() { return s_sharedPwr; }

void setSharedPower(bool on) {
  s_sharedPwr = on;
  s_prefs.putUChar("shpwr", on ? 1 : 0);
  // Applique tout de suite : le proprietaire vient peut-etre de brancher le
  // deuxieme mur, et attendre la prochaine balise serait attendre l'ecroulement.
  arenaled::setBudgetShare(on ? (uint8_t)(s_nPeers + 1) : 1);
}

// ---------------------------------------------------------------------------
//  Rang
// ---------------------------------------------------------------------------
// Ordre des MAC, voisins vivants compris. Stable d'un demarrage a l'autre, donc
// le balayage RELAY garde toujours le meme sens - ce qui compte quand les murs
// sont accroches dans un ordre physique donne.
uint8_t rank() {
  uint8_t r = 0;
  for (uint8_t i = 0; i < s_nPeers; i++)
    if (memcmp(s_peers[i].mac, s_mac, 6) < 0) r++;
  return r;
}

// ---------------------------------------------------------------------------
//  Table des voisins
// ---------------------------------------------------------------------------
static void expire() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_nPeers; ) {
    if (now - s_peers[i].seen > PEER_TTL_MS) {
      s_peers[i] = s_peers[s_nPeers - 1];
      s_nPeers--;
    } else i++;
  }
}

static Peer* upsert(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < s_nPeers; i++)
    if (!memcmp(s_peers[i].mac, mac, 6)) return &s_peers[i];
  if (s_nPeers >= MAX_PEERS) return nullptr;
  Peer* p = &s_peers[s_nPeers++];
  memset(p, 0, sizeof(*p));
  memcpy(p->mac, mac, 6);
  return p;
}

String json() {
  String j = "[";
  for (uint8_t i = 0; i < s_nPeers; i++) {
    if (i) j += ",";
    char ip[20];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)(s_peers[i].ip & 0xFF), (unsigned)((s_peers[i].ip >> 8) & 0xFF),
             (unsigned)((s_peers[i].ip >> 16) & 0xFF), (unsigned)((s_peers[i].ip >> 24) & 0xFF));
    j += "{\"name\":\"" + String(s_peers[i].name) + "\"";
    j += ",\"ip\":\"" + String(ip) + "\"";
    j += ",\"mode\":\"" + String(arenaled::modeName((arenaled::Mode)s_peers[i].mode)) + "\"";
    j += ",\"bright\":" + String(s_peers[i].bright);
    j += ",\"link\":\"" + String(linkName((Link)s_peers[i].link)) + "\"}";
  }
  return j + "]";
}

// ---------------------------------------------------------------------------
//  Emission
// ---------------------------------------------------------------------------
static void send() {
  if (s_sock < 0) return;
  Beacon b;
  memset(&b, 0, sizeof(b));
  memcpy(b.magic, "PFW1", 4);
  b.ver    = 1;
  b.link   = (uint8_t)s_link;
  b.mode   = (uint8_t)arenaled::mode();
  b.bright = arenaled::brightness();
  memcpy(b.mac,  s_mac,  6);
  memcpy(b.omac, s_omac, 6);
  b.oseq   = s_oseq;
  strncpy(b.name, arenanet::wallName().c_str(), sizeof(b.name) - 1);

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family      = AF_INET;
  to.sin_port        = htons(PORT);
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  sendto(s_sock, &b, sizeof(b), 0, (struct sockaddr*)&to, sizeof(to));
}

// ---------------------------------------------------------------------------
//  Reception
// ---------------------------------------------------------------------------
// L'etat porte par une balise n'est adopte que s'il est STRICTEMENT plus
// recent que le notre. Sans cette comparaison, deux murs se renverraient le
// meme changement indefiniment. Les egalites d'horloge sont tranchees par la
// MAC, arbitraire mais totalement ordonnee, donc les deux murs concluent
// toujours pareil et personne ne reste en arriere.
static bool newer(const Beacon& b) {
  if (b.oseq != s_oseq) return b.oseq > s_oseq;
  return memcmp(b.omac, s_omac, 6) < 0;
}

static void adopt(uint8_t mode, uint8_t bright) {
  arenaled::setMode((arenaled::Mode)mode);
  arenaled::setBrightness(bright);
  // On note l'etat comme deja vu : sinon le detecteur de changement local le
  // prendrait pour une decision de ce mur-ci et repartirait un tour d'horloge.
  s_lastMode   = mode;
  s_lastBright = bright;
}

static void receive() {
  if (s_sock < 0) return;
  Beacon b;
  struct sockaddr_in from;
  socklen_t fl = sizeof(from);
  for (int guard = 0; guard < 8; guard++) {          // borne : le tick reste court
    int n = recvfrom(s_sock, &b, sizeof(b), MSG_DONTWAIT,
                     (struct sockaddr*)&from, &fl);
    if (n != (int)sizeof(b)) return;
    if (memcmp(b.magic, "PFW1", 4) || b.ver != 1) continue;
    if (!memcmp(b.mac, s_mac, 6)) continue;          // notre propre diffusion

    Peer* p = upsert(b.mac);
    if (p) {
      strncpy(p->name, b.name, sizeof(p->name) - 1);
      p->name[sizeof(p->name) - 1] = 0;
      p->ip     = from.sin_addr.s_addr;
      p->mode   = b.mode;
      p->bright = b.bright;
      p->link   = b.link;
      p->seen   = millis();
    }

    // La synchronisation demande l'accord des DEUX murs. Un mur regle sur OFF
    // n'impose rien et ne subit rien : c'est ce qui rend le reglage sur.
    if (s_link == LINK_OFF || b.link == LINK_OFF) continue;
    if (!newer(b)) continue;

    s_oseq = b.oseq;
    memcpy(s_omac, b.omac, 6);

    if (s_link == LINK_RELAY && rank() > 0) {
      s_pending    = true;
      s_pendingAt  = millis() + (uint32_t)rank() * RELAY_STEP_MS;
      s_pendMode   = b.mode;
      s_pendBright = b.bright;
    } else {
      adopt(b.mode, b.bright);
    }
    s_burst = BURST_N;                 // relayer aux murs qui n'entendent pas l'auteur
    s_nextBurst = 0;
  }
}

// ---------------------------------------------------------------------------
//  Cycle de vie
// ---------------------------------------------------------------------------
void resetAll() {
  s_prefs.clear();
  s_link = LINK_OFF;
  Serial.println("[peers] reglage de liaison efface");
}

void begin() {
  s_prefs.begin("arenapeer", false);
  s_link = (Link)s_prefs.getUChar("link", (uint8_t)LINK_OFF);
  s_sharedPwr = s_prefs.getUChar("shpwr", 0) != 0;
  esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
  memcpy(s_omac, s_mac, 6);
}

static bool openSock() {
  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) return false;
  int one = 1;
  setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family      = AF_INET;
  me.sin_port        = htons(PORT);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s_sock, (struct sockaddr*)&me, sizeof(me)) < 0) {
    close(s_sock);
    s_sock = -1;
    return false;
  }
  Serial.printf("[peers] a l'ecoute sur %u, liaison=%s\n", PORT, linkName(s_link));
  return true;
}

void tick() {
  // Sans adresse, il n'y a pas de reseau local ou diffuser.
  const char* ip = arenanet::ip();
  if (!ip || !strcmp(ip, "0.0.0.0")) return;
  if (s_sock < 0 && !openSock()) return;

  uint32_t now = millis();
  receive();
  expire();

  // Le nombre de murs bouge : un mur qu'on debranche doit rendre sa part de
  // courant aux autres, sinon la chaine reste bridee sans raison.
  if (s_sharedPwr) arenaled::setBudgetShare((uint8_t)(s_nPeers + 1));

  if (s_pending && (int32_t)(now - s_pendingAt) >= 0) {
    s_pending = false;
    adopt(s_pendMode, s_pendBright);
  }

  // Changement decide ICI : on avance l'horloge et on s'en declare l'auteur.
  uint8_t m = (uint8_t)arenaled::mode();
  uint8_t br = arenaled::brightness();
  if (s_lastMode == 255) { s_lastMode = m; s_lastBright = br; }
  else if (m != s_lastMode || br != s_lastBright) {
    s_lastMode = m;
    s_lastBright = br;
    s_oseq++;
    memcpy(s_omac, s_mac, 6);
    s_burst = BURST_N;
    s_nextBurst = 0;
  }

  if (s_burst && (int32_t)(now - s_nextBurst) >= 0) {
    send();
    s_burst--;
    s_nextBurst = now + BURST_MS;
    s_nextBeacon = now + BEACON_MS;
    return;
  }
  if ((int32_t)(now - s_nextBeacon) >= 0) {
    send();
    s_nextBeacon = now + BEACON_MS;
  }
}

}  // namespace arenapeers
