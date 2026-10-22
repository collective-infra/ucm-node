# CMakeLists.txt -- ESP-IDF component registration for the UCM sensor node
# firmware.
#
# This is the component-level build file (not the top-level project
# CMakeLists.txt) that tells the ESP-IDF build system which source files
# make up this firmware, where its own headers live, and which other
# ESP-IDF components/managed dependencies it needs to link against.
#
# WHAT EACH SOURCE FILE CONTRIBUTES (for context when reading SRCS below):
#   - node_main.c        app_main() entry point; sensor task + boot sequence.
#   - sen54_data.c        Shared sensor/node-identity data definitions.
#   - web_server.c         Local HTTP status/API server.
#   - app_wifi_prov.c      Wi-Fi/network provisioning + stored node identity.
#   - ethernet_manager.c   Higher-level Ethernet connection management.
#   - ethernet_init.c      Low-level Ethernet PHY/MAC bring-up.
#   - network_manager.c    Chooses/coordinates between transports and
#                           exposes network_start() used by node_main.c.
#   - mqtt_manager.c        MQTT client lifecycle, topics, and publishes.
#
# Note: the SEN5x Sensirion sensor driver itself is NOT listed as a source
# file here -- it's pulled in as the "sen5x" component via PRIV_REQUIRES
# below, presumably vendored/managed elsewhere in the project (e.g. as a
# separate component or managed component dependency) rather than compiled
# directly as part of this component.

idf_component_register(
    # Every .c file that is compiled as part of this firmware component.
    # If a new .c file is added to the project, it must be added here or
    # ESP-IDF's build system will not compile it in.

    SRCS "node_main.c" "sen54_data.c" "web_server.c" "app_wifi_prov.c" "ethernet_manager.c" "ethernet_init.c" "network_manager.c" "mqtt_manager.c" "ota_manager.c" "web_update.c" 

    # Directories searched for this component's own headers (e.g.
    # sen54_data.h, web_server.h, mqtt_manager.h, app_wifi_prov.h). "."
    # means the component's own root directory, i.e. headers live alongside
    # their corresponding .c files rather than in a separate include/
    # subdirectory.
    INCLUDE_DIRS "."

    # Other ESP-IDF components this firmware depends on, but only
    # privately -- i.e. these headers/APIs are used internally by this
    # component's own .c files and are not required by (or exposed to) any
    # other component that might depend on this one. (Compare with the
    # unused REQUIRES keyword, which would expose these as public/transitive
    # dependencies -- not needed here since nothing currently depends on
    # this component as a library.)
    PRIV_REQUIRES
        nvs_flash        # Non-volatile storage -- used for Wi-Fi/provisioning config persistence (node_main.c, app_wifi_prov.c)
        sen5x             # Sensirion SEN5x sensor driver -- used by node_main.c for I2C sensor readings
        esp_wifi           # Wi-Fi driver/API -- used for provisioning and (in mqtt_manager.c) reading RSSI via esp_wifi_sta_get_ap_info()
        esp_event           # Event loop framework -- used for MQTT client event registration (mqtt_manager.c) and Wi-Fi/network events
        esp_netif   
        esp_system            # Core system APIs -- e.g. esp_restart(), MAC address access (node_main.c, mqtt_manager.c)
        esp_eth                # Ethernet driver support -- used by ethernet_manager.c / ethernet_init.c
	esp_http_server           # HTTP server framework -- used by web_server.c
        esp_http_client
        esp_https_ota
	mdns                       # mDNS resolution -- required to resolve the "UCM-HUB.local" MQTT broker hostname (mqtt_manager.c)
        log                         # ESP_LOG* logging macros -- used throughout every source file in this component
        i2cdev                       # esp-idf-lib I2C device abstraction -- used by node_main.c's SEN5x I2C bring-up
        app_update
        bootloader_support
)