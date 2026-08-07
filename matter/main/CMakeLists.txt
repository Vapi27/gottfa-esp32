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

# Les bibliotheques par GLOB, et versees dans SRCS.
# Piege d'ESP-IDF : quand SRCS est fourni, SRC_DIRS est **purement ignore**,
# sans un mot. Le CMakeLists donnait les deux, donc ESPAsyncWebServer n'etait
# plus compile du tout - cinq references non resolues a l'edition de liens, dont
# le constructeur d'AsyncWebServer, apres une reconstruction complete de CHIP.
file(GLOB ARENA_LIB_SRCS
     "../libs/AsyncTCP/src/*.cpp"
     "../libs/ESPAsyncWebServer/src/*.cpp"
     "../../lib/Adafruit_NeoPixel/*.cpp"  "../../lib/Adafruit_NeoPixel/*.c"
     "../../lib/Adafruit_GFX/*.cpp"       "../../lib/Adafruit_GFX/*.c"
     "../../lib/Adafruit_SSD1306/*.cpp"
     "../../lib/Adafruit_BusIO/*.cpp")

idf_component_register(
    SRCS "arena_matter.cpp"
         "${ARENA_SRC}/arena_main.cpp"
         "${ARENA_SRC}/arenaled.cpp"
         "${ARENA_SRC}/arena_net.cpp"
         "${ARENA_SRC}/arena_pf.cpp"
         "${ARENA_SRC}/arena_map.cpp"
         "${ARENA_SRC}/arena_attract.cpp"
         "${ARENA_SRC}/arena_oled.cpp"
         "${ARENA_SRC}/arena_peers.cpp"
         ${ARENA_LIB_SRCS}
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

# Garde-fou sur les corrections portees dans les sources tierces.
#
# Ces deux fichiers sont versionnes ici et modifies : sans le premier correctif
# la chaine LED reste noire par intermittence, sans le second un mutex fuit et
# la cadence tombe a 20 images/s. Les deux ont deja regresse silencieusement en
# passant inapercus dans une mise a jour de bibliotheque. Un echec de
# construction coute une minute ; un mur noir chez un client coute davantage.
foreach(GUARD
        "../components/arduino-esp32/cores/esp32/esp32-hal-rmt.c|allow_pd"
        "../../lib/Adafruit_NeoPixel/esp.c|espShowRmtFail")
  string(REPLACE "|" ";" G "${GUARD}")
  list(GET G 0 GFILE)
  list(GET G 1 GNEEDLE)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/${GFILE}" GTEXT)
  string(FIND "${GTEXT}" "${GNEEDLE}" GPOS)
  if(GPOS EQUAL -1)
    message(FATAL_ERROR
      "Correction perdue : '${GNEEDLE}' absent de ${GFILE}. "
      "Voir matter/patches/README.md avant de continuer.")
  endif()
endforeach()
