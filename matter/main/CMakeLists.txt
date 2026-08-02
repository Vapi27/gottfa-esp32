idf_component_register(SRC_DIRS "." "../libs/AsyncTCP/src" "../libs/ESPAsyncWebServer/src" "../libs/Adafruit_NeoPixel"
                       INCLUDE_DIRS "." "../libs/AsyncTCP/src" "../libs/ESPAsyncWebServer/src" "../libs/ArduinoJson/src" "../libs/Adafruit_NeoPixel"
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils")
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H" "-DARENA_MATTER" "-DARENA_BOARD_D1MINI32" "-DARENA_LED")
