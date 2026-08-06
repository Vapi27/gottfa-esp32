// ============================================================================
//  Matter bridge — a master lamp, plus one switch per mode.
//
//  Endpoint 1 is a dimmable light: On/Off is the wall itself, Level is
//  brightness. Then one on/off plug-in unit per animation mode, because that is
//  the only shape Apple Home and Siri actually surface: a named thing you turn
//  on. Matter has no "pick a mode" control any voice assistant renders, so a
//  mode becomes a switch, and the switches behave like radio buttons - turning
//  one on turns the others off.
//
//  The owner renames them in the Home app; whatever they type there is what
//  Siri listens for. We deliberately do not fight over the names.
//
//  WiFi belongs to CHIP from commissioning onward — arena_net is compiled with
//  ARENA_MATTER and only waits for the address.
// ============================================================================
#ifdef ARENA_MATTER
// Arduino frees the BT controller RAM at startup when the sketch shows no BT
// usage (weak btInUse() returns false) - which strands CHIP: its BLE init then
// fails with 'already released' and commissioning is impossible. Claim it.
extern "C" bool btInUse() { return true; }

// With SoftAP support compiled OUT (it must be: CHIP auto-starts an AP interface
// whenever it is compiled IN, and the extra beacon buffers starve the ESP32
// classic - measured: alloc eb fail then LoadProhibited), Arduino WiFiAP still
// references this symbol. It is unreachable under the ARENA_MATTER guard, which
// never calls WiFi.softAP; a stub satisfies the linker.
#include <esp_netif.h>
extern "C" esp_netif_t *esp_netif_create_default_wifi_ap(void) { return NULL; }

#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <app/server/Server.h>   // FabricTable : a combien de maisons appartient-on
// The only file that sees BOTH worlds: CHIP drags lwip in first, and lwip
// #defines INADDR_NONE/INADDR_ANY as numeric macros - which then shred the
// 'extern const IPAddress INADDR_NONE;' declarations in the Arduino core
// pulled by arenaled.h. Undefine between the two, the canonical cure.
#undef INADDR_NONE
#undef INADDR_ANY
#include "arenaled.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static uint16_t s_light_ep = 0;

// Un endpoint par mode. TEST est volontairement absent : c'est un outil de
// diagnostic de cablage, pas une ambiance, et personne ne veut le declencher
// a la voix par erreur. OFF non plus : c'est la lampe maitresse qu'on eteint.
struct ModeEp {
  arenaled::Mode mode;
  const char*    label;   // journal uniquement - Maison laisse l'utilisateur nommer
  uint16_t       ep;
};
static ModeEp s_modeEps[] = {
  { arenaled::MODE_ATTRACT, "Attract",     0 },
  { arenaled::MODE_CLASSIC, "Classique",   0 },
  { arenaled::MODE_ARENA,   "Arena",       0 },
  { arenaled::MODE_RAINBOW, "Arc-en-ciel", 0 },
  { arenaled::MODE_NIGHT,   "Nuit",        0 },
  { arenaled::MODE_MUSIC,   "Musique",     0 },
};
static constexpr size_t MODE_EP_N = sizeof(s_modeEps) / sizeof(s_modeEps[0]);

// Ce que la lampe maitresse rallume. Eteindre puis rallumer doit rendre le mode
// qu'on avait, pas un mode arbitraire.
static arenaled::Mode s_lastOn = arenaled::MODE_ATTRACT;

// attribute::update() redeclenche attribute_update_cb. Sans ce garde, reflechir
// l'etat vers Maison rappellerait setMode() en boucle.
static bool s_syncing = false;

// Au demarrage, esp-matter restaure ses attributs persistes et emet un
// PRE_UPDATE par endpoint. Ces evenements ne viennent de personne : ils
// rejouent l'etat d'avant la coupure. Les honorer annulait le rallumage
// decide par arenaled::begin() - vu le 2026-08-02 : "mev = t1 ep4 cl6 at0 v0"
// quelques secondes apres un demarrage ou le mur venait de se rallumer.
// Tant que la carte n'a pas publie SON etat (premier arena_matter_sync),
// c'est elle qui a raison, pas la memoire de Matter.
static bool s_ready = false;

// Dernier evenement d'attribut recu, publie dans /api/state. Sans ca, un
// interrupteur qui "ne fait rien" est indiscernable d'un interrupteur dont la
// commande n'arrive jamais - et on ne peut pas brancher un cable serie chez le
// client. Format : "ep<N> cl<X> at<Y> v<Z>".
static char s_lastEv[48] = "aucun";

// Historique circulaire des derniers evenements. Un seul "dernier evenement" ne
// suffit pas : quand Maison eteint une lampe variable, il envoie souvent une
// RAMPE de luminosite (cluster 8) suivie de l'extinction (cluster 6) - et
// l'extinction ecrase justement la trace de la rampe qu'on cherchait a voir.
// Avec l'historique, le proprietaire fait le test quand il veut et on lit apres.
#define ARENA_EVLOG_N 12
static char     s_evLog[ARENA_EVLOG_N][40];
static uint8_t  s_evHead = 0;
static uint32_t s_evSeq  = 0;

static esp_err_t identification_cb(identification::callback_type_t, uint16_t,
                                   uint8_t, uint8_t, void *) { return ESP_OK; }

static esp_err_t attribute_update_cb(attribute::callback_type_t type,
                                     uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void *) {
  // Journalise AVANT tout filtre : c'est le seul moyen de distinguer "la
  // commande n'arrive pas" de "elle arrive et on la jette".
  snprintf(s_lastEv, sizeof(s_lastEv), "t%d ep%u cl%lu at%lu v%u",
           (int)type, endpoint_id, (unsigned long)cluster_id,
           (unsigned long)attribute_id, (unsigned)val->val.u8);
  // On journalise AVANT les filtres (s_syncing, s_ready) et on marque d'ou ca
  // vient : "=" nos propres ecritures, ">" ce qui arrive de l'exterieur.
  snprintf(s_evLog[s_evHead], sizeof(s_evLog[0]), "%lu%c ep%u cl%lu v%u",
           (unsigned long)(s_evSeq++), s_syncing ? '=' : '>',
           endpoint_id, (unsigned long)cluster_id, (unsigned)val->val.u8);
  s_evHead = (uint8_t)((s_evHead + 1) % ARENA_EVLOG_N);

  if (type != attribute::PRE_UPDATE) return ESP_OK;
  if (s_syncing) return ESP_OK;          // c'est nous qui ecrivons, pas Maison
  if (!s_ready)  return ESP_OK;          // restauration de demarrage : voir s_ready

  const bool isOnOff = (cluster_id == OnOff::Id &&
                        attribute_id == OnOff::Attributes::OnOff::Id);

  // --- un interrupteur de mode ---
  for (size_t i = 0; i < MODE_EP_N; i++) {
    if (endpoint_id != s_modeEps[i].ep) continue;
    if (!isOnOff) return ESP_OK;
    // Eteindre un mode eteint le mur : dans Maison il n'y a pas d'autre geste
    // disponible, et laisser le mode en place ferait un interrupteur qui ment.
    arenaled::setMode(val->val.b ? s_modeEps[i].mode : arenaled::MODE_OFF);
    return ESP_OK;                        // le reste est reflechi par arena_matter_sync()
  }

  // --- la lampe maitresse ---
  if (endpoint_id != s_light_ep) return ESP_OK;
  if (isOnOff) {
    arenaled::setMode(val->val.b ? s_lastOn : arenaled::MODE_OFF);
  } else if (cluster_id == LevelControl::Id &&
             attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
    arenaled::setBrightness((uint8_t)(((int)val->val.u8 * 255) / 254));
  }
  return ESP_OK;
}

// Reflechit l'etat reel vers Maison. Appele depuis la boucle principale, donc
// un changement fait sur la page web ou par /api/set remonte aussi a Siri : les
// interrupteurs de Maison doivent dire la verite, pas seulement la recevoir.
void arena_matter_sync() {
  static arenaled::Mode last  = arenaled::MODE_COUNT;  // force une premiere passe
  static int            lastB = -1;
  const arenaled::Mode m = arenaled::mode();
  const uint8_t        b = arenaled::brightness();

  // La luminosite doit remonter vers Maison, pas seulement en descendre. Sans
  // ca, Matter garde sa propre idee du niveau - mesure le 2026-08-03 : il en
  // etait reste a 1 pendant que le mur tournait a 162. Un "monte la luminosite"
  // se calcule alors depuis un etat faux, et l'assistant annonce un changement
  // que le mur ne fait pas.
  if ((int)b != lastB) {
    lastB = (int)b;
    s_syncing = true;
    // Matter compte de 1 a 254 ; 0 n'est pas un niveau, c'est "eteint".
    uint8_t lv = (uint8_t)(((int)b * 254) / 255);
    if (lv < 1) lv = 1;
    esp_matter_attr_val_t v = esp_matter_nullable_uint8(nullable<uint8_t>(lv));
    attribute::update(s_light_ep, LevelControl::Id,
                      LevelControl::Attributes::CurrentLevel::Id, &v);
    s_syncing = false;
  }

  if (m == last) { s_ready = true; return; }
  last = m;
  if (m != arenaled::MODE_OFF) s_lastOn = m;

  s_syncing = true;
  for (size_t i = 0; i < MODE_EP_N; i++) {
    esp_matter_attr_val_t v = esp_matter_bool(s_modeEps[i].mode == m);
    attribute::update(s_modeEps[i].ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &v);
  }
  esp_matter_attr_val_t on = esp_matter_bool(m != arenaled::MODE_OFF);
  attribute::update(s_light_ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
  s_syncing = false;
  // Notre etat est publie : a partir d'ici, ce qui arrive vient vraiment de
  // l'utilisateur et doit etre honore.
  s_ready = true;
}

static void event_cb(const ChipDeviceEvent *event, intptr_t) {
  // Freeze the wall while a phone holds a BLE link: pairing crypto gets the
  // whole chip, and the strip resumes the instant the link drops.
  switch (event->Type) {
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished:
    arenaled::setPaused(true);
    break;
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionClosed:
  // Ces deux-la parce que le precedent ne vient pas toujours : une fois le
  // commissioning termine, CHIP demonte BLE sans annoncer la fermeture.
  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
  case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    arenaled::setPaused(false);
    break;
  default:
    break;
  }
}

void arena_matter_init() {
  node::config_t node_config;
  node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
  endpoint::dimmable_light::config_t light_config;
  light_config.on_off.on_off = true;
  endpoint_t *ep = endpoint::dimmable_light::create(node, &light_config,
                                                    ENDPOINT_FLAG_NONE, NULL);
  s_light_ep = endpoint::get_id(ep);

  // Un interrupteur par mode. Ils apparaissent dans Maison comme des tuiles
  // distinctes du meme accessoire ; l'utilisateur les renomme, et ce nom devient
  // la phrase que Siri reconnait.
  for (size_t i = 0; i < MODE_EP_N; i++) {
    endpoint::on_off_plug_in_unit::config_t cfg;
    cfg.on_off.on_off = (s_modeEps[i].mode == arenaled::mode());
    endpoint_t *me = endpoint::on_off_plug_in_unit::create(node, &cfg,
                                                           ENDPOINT_FLAG_NONE, NULL);
    s_modeEps[i].ep = me ? endpoint::get_id(me) : 0;
    ESP_LOGI("arena", "mode '%s' -> endpoint %u", s_modeEps[i].label, s_modeEps[i].ep);
  }

  esp_matter::start(event_cb);
}

// --- Ponts vers arena_net.cpp -----------------------------------------------
// arena_net n'inclut volontairement AUCUN en-tete Matter (lwip y redefinit
// INADDR_* et massacre les declarations Arduino, cf. le #undef plus haut).
// Ces deux fonctions sont donc la seule passerelle.

// Nombre de fabrics commissionnees. 0 = la carte n'est dans aucune maison ;
// Maison qui repond "deja dans une autre maison" alors que ce compteur vaut 0
// serait une incoherence a creuser cote iPhone, pas cote carte.
extern "C" const char* arena_matter_last_event() { return s_lastEv; }

// Les evenements du plus ancien au plus recent, separes par des virgules.
extern "C" void arena_matter_event_log(char* out, size_t n) {
  if (!out || !n) return;
  out[0] = 0;
  size_t used = 0;
  for (uint8_t k = 0; k < ARENA_EVLOG_N; k++) {
    const char* e = s_evLog[(s_evHead + k) % ARENA_EVLOG_N];
    if (!e[0]) continue;
    const size_t l = strlen(e);
    if (used + l + 2 >= n) break;
    if (used) { out[used++] = ','; }
    memcpy(out + used, e, l); used += l; out[used] = 0;
  }
}

extern "C" uint8_t arena_matter_fabrics() {
  return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

// Oublie TOUT : fabrics Matter, identifiants WiFi, et la NVS avec eux. La carte
// redemarre en mode appairage et disparait du reseau jusqu'a ce qu'un telephone
// la reprenne en Bluetooth. A n'appeler que si l'accessoire est introuvable ou
// irrecuperable dans Maison - il n'y a pas de retour en arriere sans telephone.
extern "C" void arena_matter_forget() {
  esp_matter::factory_reset();
}
#endif
