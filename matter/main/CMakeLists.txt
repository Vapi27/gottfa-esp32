# The Matter firmware compiles the SAME sources as the PlatformIO build.
#
# These files used to exist twice — once in src/, once here — and the two copies
# drifted by ~684 lines, 491 of them in arena_net.cpp alone. Every fix had to be
# written twice, and by 2026-08-04 several had only been written once: the wall's
# firmware had the chunked uploader and the WiFi scan, the PlatformIO build had
# the OLED screen, and neither had the other. That is why there is now exactly
# one copy of every shared file, in src/ and include/.
#
# SRCS is spelled out rather than using SRC_DIRS "../../src": that directory also
# holds the GottFA80+ application (a 6502 emulator, sound engines, an SD stack),
# and none of it belongs in a wall-art firmware.
#
# arena_matter.cpp stays here. It is the only file that sees both worlds — CHIP
# drags lwip in first, and lwip's INADDR_* macros shred the Arduino IPAddress
# declarations, so it carries #undefs no other file needs.
set(ARENA_SRC "../../src")

idf_component_register(
    SRCS "arena_matter.cpp"
         "${ARENA_SRC}/arena_main.cpp"
         "${ARENA_SRC}/arenaled.cpp"
         "${ARENA_SRC}/arena_net.cpp"
         "${ARENA_SRC}/arena_pf.cpp"
         "${ARENA_SRC}/arena_map.cpp"
         "${ARENA_SRC}/arena_attract.cpp"
         "${ARENA_SRC}/arena_oled.cpp"
    # SRC_DIRS pour les bibliotheques : ce sont des repertoires feuilles, tout ce
    # qu'ils contiennent leur appartient. Les enumerer fichier par fichier a
    # coute une serie d'erreurs de lien - il manquait Middleware.cpp et deux
    # autres. SRCS reste reserve a src/, qui heberge aussi l'application GottFA.
    SRC_DIRS "../libs/AsyncTCP/src" "../libs/ESPAsyncWebServer/src"
             "../../lib/Adafruit_NeoPixel" "../../lib/Adafruit_GFX"
             "../../lib/Adafruit_SSD1306" "../../lib/Adafruit_BusIO"
    INCLUDE_DIRS "." "${ARENA_SRC}" "../../include" "../libs/ESPAsyncWebServer/src" "../../lib/Adafruit_NeoPixel"
                 "../../lib/Adafruit_GFX" "../../lib/Adafruit_SSD1306" "../../lib/Adafruit_BusIO"
                 "../libs/AsyncTCP/src" "../libs/ArduinoJson/src"
    PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils")

# ARENA_MATTER is what selects the Matter half of every #ifdef in the shared
# files: CHIP owns WiFi, no SoftAP, no ESPmDNS, the web server deferred until an
# address exists. Never pass a board define here — an earlier CMakeLists still
# forced -DARENA_BOARD_D1MINI32, which selects the WROOM pin map and would have
# driven the LED chain on GPIO27 of an S3.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-DCHIP_HAVE_CONFIG_H" "-DARENA_MATTER" "-DARENA_LED")
