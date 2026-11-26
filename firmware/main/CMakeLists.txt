# every file in sensors/ is a sensor (a file whose name starts with "_" is a template and is not built)
file(GLOB SENSOR_SRCS "sensors/*.c")
list(FILTER SENSOR_SRCS EXCLUDE REGEX "/_[^/]*$")
idf_component_register(
    SRCS "main.c" "model.c" "board.c" "camera.c" "audio.c" "mel.c" "ego.c" "decide.cc" "actions.c" "console.c" "radio.c"
         "rules.c" "store.c" "wifi.c" "web.c" "alert.c" "devices.c" "home.c" "push.c" "inputs.c" "eye.cc" "power.c" "update.c" ${SENSOR_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag esp_partition
                  esp_timer esp_psram nvs_flash json mbedtls esp_rom esp_wifi esp_netif esp_event
                  esp_http_server esp_http_client esp-tls mqtt lwip app_update bootloader_support esp_app_format esp_driver_tsens spi_flash vfs esp_adc
    EMBED_TXTFILES "www/index.html" "recipes.json"
    WHOLE_ARCHIVE                 # a sensor file registers itself; nothing else names it, so the linker must keep it
)
# the fly headers keep everything in one file each: some of it is not used here
set_source_files_properties(eye.cc PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-misleading-indentation;-Wno-parentheses")
# snprintf into fixed buffers is meant to cut long text short
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
