// ============================================================================
//  Matter bridge — the wall as a dimmable light.
//
//  The assistants see a lamp: On/Off maps to attract/off, Level to brightness.
//  The fine modes stay on the web page; Matter is the coarse control every
//  ecosystem understands. WiFi belongs to CHIP from commissioning onward —
//  arena_net is compiled with ARENA_MATTER and only waits for the address.
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
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
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

static esp_err_t identification_cb(identification::callback_type_t, uint16_t,
                                   uint8_t, uint8_t, void *) { return ESP_OK; }

static esp_err_t attribute_update_cb(attribute::callback_type_t type,
                                     uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void *) {
  if (type != attribute::PRE_UPDATE || endpoint_id != s_light_ep) return ESP_OK;
  if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
    arenaled::setMode(val->val.b ? arenaled::MODE_ATTRACT : arenaled::MODE_OFF);
  } else if (cluster_id == LevelControl::Id &&
             attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
    arenaled::setBrightness((uint8_t)(((int)val->val.u8 * 255) / 254));
  }
  return ESP_OK;
}

static void event_cb(const ChipDeviceEvent *event, intptr_t) {
  // Freeze the wall while a phone holds a BLE link: pairing crypto gets the
  // whole chip, and the strip resumes the instant the link drops.
  switch (event->Type) {
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished:
    arenaled::setPaused(true);
    break;
  case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionClosed:
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
  esp_matter::start(event_cb);
}
#endif
