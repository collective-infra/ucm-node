/**
 * @file node_main.c
 * @brief Main application entry point for the UCM environmental sensor
 *        node firmware.
 *
 * TARGET HARDWARE
 * ----------------
 * - ESP32-P4 dev board (network connectivity brought up via
 *   network_manager.h / app_wifi_prov.h -- this could be Wi-Fi and/or
 *   Ethernet/PoE depending on how network_start() is implemented; this
 *   file itself is agnostic to which transport is used).
 * - Sensirion SEN54/SEN5x environmental sensor (particulate matter,
 *   temperature, humidity, VOC index) wired over I2C.
 *
 * WHAT THIS FILE DOES, END TO END
 * --------------------------------
 * 1. Initializes NVS (non-volatile storage), required by the network/
 *    provisioning stack to persist configuration data.
 * 2. Initializes the SEN5x sensor over I2C and starts continuous
 *    measurement mode -- done before networking so sensor bring-up isn't
 *    gated on/blocked by the network coming up.
 * 3. Brings up the network connection (network_start()). Only if this
 *    succeeds does the rest of the node's identity/reporting pipeline run:
 *      - Synchronizes the system clock via SNTP.
 *      - Loads this node's stored identity/location (name, description,
 *        GPS lat/lon) via app_wifi_prov_get_node_info().
 *      - Derives a stable, unique node ID from the device's MAC address.
 *      - Starts the MQTT manager and publishes a one-time node-info
 *        message describing this node's identity to the broker.
 * 4. Starts a local web server (see web_server.h) for local access to the
 *    node (e.g. a status page / API) -- this happens unconditionally,
 *    regardless of whether the network/MQTT setup above succeeded.
 * 5. Launches a background task that polls the sensor once per second and
 *    publishes both an environment reading and a heartbeat message to
 *    MQTT via the mqtt_manager module.
 *
 * This is the corrected/current version of the node firmware -- it fixes
 * an issue present in an earlier draft (wifi_test.c) where the network
 * connection result was checked using an undeclared variable name. Here,
 * `network_result` is consistently used both when it's set and when it's
 * checked, and all node-identity/MQTT setup is correctly gated on that
 * same result via the surrounding if/else block.
 *
 * NOTE FOR NEW CONTRIBUTORS: this file is largely AI-generated glue code
 * tying together several ESP-IDF components, a project-specific
 * network/MQTT manager layer, and Sensirion's vendor sensor driver. The
 * comments below explain *why* each piece exists, not just what it does.
 */

#include <string.h>
#include <stdbool.h>
#include <time.h>

#include <stdio.h>
#include <stdint.h>

#include "ota_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"          // Non-volatile storage, required by the network/provisioning stack

#include "esp_log.h"
#include "esp_mac.h"             // Provides esp_efuse_mac_get_default(), used to derive the node ID
#include "esp_netif_sntp.h"      // SNTP client for time synchronization

#include "network_manager.h"     // Project-specific wrapper that brings up network connectivity (network_start())
#include "app_wifi_prov.h"       // Wi-Fi provisioning + stored node identity (name/desc/lat/lon)
#include "esp_http_server.h"

#include "sen54_data.h"          // Shared struct holding the latest sensor + node metadata (current_sensor_data)
#include "sen5x_i2c.h"           // Sensirion SEN5x high-level driver API
#include "sensirion_i2c_hal.h"
#include "sensirion_i2c_esp32_config.h"

#include "i2cdev.h"              // esp-idf-lib I2C device abstraction used by the Sensirion HAL

#include "web_server.h"          // Local HTTP server for this node (status/API)
#include "mqtt_manager.h"        // Project-specific wrapper around the MQTT client (connect, publish helpers)



// ---------------------------------------------------------------------------
// SEN5x I2C wiring / bus configuration
// ---------------------------------------------------------------------------
// These pins and the I2C address are specific to how the SEN54 is wired to
// the ESP32-P4 dev board on this project. 0x69 is the SEN5x's fixed I2C
// address (not configurable on the sensor itself).
#define SEN5X_SDA      GPIO_NUM_7
#define SEN5X_SCL      GPIO_NUM_8
#define SEN5X_I2C_PORT I2C_NUM_0
#define SEN5X_ADDRESS  0x69

// Formerly a hardcoded broker URI used directly with the MQTT client in an
// earlier version of this file; broker connection details are now handled
// inside mqtt_manager (see mqtt_manager_start()), so this is left here,
// disabled, presumably as a reminder of the prior configuration.
// #define MQTT_BROKER_URI "mqtt://192.168.1.236"

// Declared but not currently referenced anywhere in this file -- likely
// intended as a timeout to bound how long network_start() (or an
// underlying Ethernet link-up step) is allowed to take, for future use
// inside network_manager.
#define ETHERNET_TIMEOUT_MS 30000

static const char *TAG = "P4_SENSOR_TEST";


/**
 * @brief Derives a stable, unique node ID from the board's MAC address, e.g.
 *        "UCM-A1B2C3" -- guaranteed different per physical device, no manual
 *        config needed, and stable across reboots since MAC doesn't change.
 *
 * @param out       Destination buffer for the generated ID string.
 * @param out_size  Size of the destination buffer (passed to snprintf to
 *                  avoid overflow).
 *
 * Only the last 3 MAC bytes are used (mac[3..5]) since that's the portion
 * that varies per-device within a given vendor OUI block, keeping the
 * resulting ID short and human-friendly. Unlike the earlier draft of this
 * function, the MAC read here is wrapped in ESP_ERROR_CHECK(), so a failure
 * to read the MAC (which should not normally happen) will abort boot
 * instead of silently proceeding with an uninitialized `mac` buffer.
 */
static void generate_node_id(char *out, size_t out_size)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));

    snprintf(out,
             out_size,
             "UCM-%02X%02X%02X",
             mac[3],
             mac[4],
             mac[5]);
}


/**
 * @brief Synchronizes the device's system clock over SNTP against
 *        pool.ntp.org, blocking for up to 10 seconds.
 *
 * Accurate time is needed so that sensor readings published to MQTT can
 * carry a meaningful UTC timestamp (see format_timestamp()). If sync fails
 * or times out, firmware continues running anyway (it's a warning, not a
 * fatal error) -- but published timestamps will be wrong/unset until a
 * later sync succeeds, since there's no retry logic here. This is only
 * called once the network has successfully come up (see app_main()),
 * since SNTP requires a working network connection.
 */
static void time_sync_start(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    ESP_LOGI(TAG, "Waiting for time sync...");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Time sync failed/timed out -- timestamps will be inaccurate");
    } else {
        ESP_LOGI(TAG, "Time synced");
    }
}

/**
 * @brief Formats the current system time as an ISO-8601 / RFC 3339 UTC
 *        timestamp string, e.g. "2026-08-08T14:32:01Z".
 *
 * @param out       Destination buffer for the formatted string.
 * @param out_size  Size of the destination buffer.
 *
 * Relies on the clock having been set via time_sync_start(); if SNTP sync
 * never succeeded (or the network never came up at all), this will format
 * whatever the device's default/incorrect clock value is.
 */
static void format_timestamp(char *out, size_t out_size)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
}

/**
 * @brief Brings up the I2C bus and the SEN5x sensor, then starts continuous
 *        measurement mode.
 *
 * Steps:
 *  1. i2cdev_init() -- initializes esp-idf-lib's I2C device layer, which
 *     the Sensirion ESP32 HAL config (sensirion_i2c_esp32_config.h) sits on
 *     top of.
 *  2. sensirion_i2c_config_esp32() -- tells the Sensirion HAL which pins,
 *     I2C port, address, and bus speed to use (see SEN5X_* defines above).
 *  3. sensirion_i2c_hal_init() -- initializes Sensirion's generic I2C HAL,
 *     which sen5x_device_reset()/sen5x_start_measurement() etc. use
 *     internally to talk to the sensor.
 *  4. sensirion_i2c_esp32_ok() -- sanity check that the ESP32-specific HAL
 *     config succeeded; ESP_ERROR_CHECK'd, so this aborts boot on failure.
 *  5. sen5x_device_reset() -- soft-resets the sensor to a known state.
 *  6. sen5x_start_measurement() -- puts the sensor into continuous
 *     measurement mode so that sen5x_read_measured_values() (used later in
 *     sen5x_task()) will return valid data.
 *
 * Note: unlike step 4, failures in reset/start-measurement (steps 5-6) are
 * only logged, not treated as fatal -- the function just returns early,
 * leaving the sensor task to encounter read errors later.
 */
static void sen5x_init(void)
{
    ESP_ERROR_CHECK(i2cdev_init());
    struct esp32_i2c_config i2c_cfg = {
        .freq = 100000,
        .addr = SEN5X_ADDRESS,
        .port = SEN5X_I2C_PORT,
        .sda = SEN5X_SDA,
        .scl = SEN5X_SCL,
        .sda_pullup = false,
        .scl_pullup = false,
    };

    ESP_ERROR_CHECK(sensirion_i2c_config_esp32(&i2c_cfg));
    sensirion_i2c_hal_init();
    ESP_ERROR_CHECK(sensirion_i2c_esp32_ok());

    int16_t error = sen5x_device_reset();
    if (error) {
        ESP_LOGE(TAG, "SEN5x reset failed: %d", error);
        return;
    }
    ESP_LOGI(TAG, "SEN5x reset successful");

    error = sen5x_start_measurement();
    if (error) {
        ESP_LOGE(TAG, "SEN5x start measurement failed: %d", error);
        return;
    }
    ESP_LOGI(TAG, "SEN5x measurement started");
}

/**
 * @brief Background FreeRTOS task: polls the SEN5x sensor once per second,
 *        logs the readings, updates the shared current_sensor_data struct,
 *        and publishes both an environment reading and a heartbeat to
 *        MQTT via the mqtt_manager module.
 *
 * @param arg  Unused (required by the xTaskCreate() task function
 *             signature).
 *
 * Units / scaling notes (per Sensirion SEN5x datasheet, values are returned
 * as fixed-point integers and must be scaled down):
 *   - PM1.0/2.5/4.0/10 are in tenths of ug/m3    -> divide by 10.0
 *   - Temperature is in 1/200 degrees C          -> divide by 200.0
 *   - Humidity is in 1/100 %RH                   -> divide by 100.0
 *   - VOC index is in tenths of index points     -> divide by 10.0
 *   - nox_index_unused is read (the driver call requires it) but not used
 *     or published anywhere in this firmware.
 *
 * current_sensor_data is a shared/global struct (declared in
 * sen54_data.h) that is also read elsewhere (e.g. by the web server) to
 * expose the latest readings -- there's no mutex/lock around these writes,
 * so this assumes readers tolerate momentarily torn/partial updates.
 *
 * Unlike the earlier draft of this firmware (which built and published raw
 * MQTT JSON payloads itself), publishing here is delegated to
 * mqtt_manager_publish_environment() and mqtt_manager_publish_heartbeat(),
 * which presumably handle topic naming, payload formatting, retain/QoS
 * flags, and "is MQTT even connected" checks internally.
 *
 * NOTE: this task is started unconditionally in app_main() (even if the
 * network/MQTT never came up), so mqtt_manager_publish_environment() /
 * mqtt_manager_publish_heartbeat() must be safe to call when MQTT isn't
 * connected -- they presumably no-op or fail gracefully in that case,
 * since there's no connectivity check here before calling them.
 */
static void sen5x_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint16_t pm1p0, pm2p5, pm4p0, pm10p0;
        int16_t humidity, temperature, voc_index, nox_index_unused;

        int16_t error = sen5x_read_measured_values(
            &pm1p0, &pm2p5, &pm4p0, &pm10p0,
            &humidity, &temperature, &voc_index, &nox_index_unused);

        if (error) {
            ESP_LOGE(TAG, "SEN5x read failed: %d", error);
            continue;
        }

        ESP_LOGI(TAG, "PM1.0=%.1f PM2.5=%.1f PM4.0=%.1f PM10=%.1f ug/m3",
                 pm1p0 / 10.0f,
                 pm2p5 / 10.0f,
                 pm4p0 / 10.0f,
                 pm10p0 / 10.0f);

        ESP_LOGI(TAG, "Temperature=%.1f C Humidity=%.1f %%RH VOC=%.1f",
                 temperature / 200.0f,
                 humidity / 100.0f,
                 voc_index / 10.0f);


        current_sensor_data.pm1 = pm1p0 / 10.0f;
        current_sensor_data.pm25 = pm2p5 / 10.0f;
        current_sensor_data.pm4 = pm4p0 / 10.0f;
        current_sensor_data.pm10 = pm10p0 / 10.0f;
        current_sensor_data.temperature = temperature / 200.0f;
        current_sensor_data.humidity = humidity / 100.0f;
        current_sensor_data.voc = voc_index / 10.0f;

        format_timestamp(
            current_sensor_data.timestamp,
            sizeof(current_sensor_data.timestamp)
        );


        // Publishes the live reading (non-retained -- see mqtt_manager for
        // topic/retain/QoS details) so subscribers get fresh data every
        // second.
        mqtt_manager_publish_environment(
            &current_sensor_data
        );
        // Separate heartbeat publish -- lets downstream consumers /
        // dashboards distinguish "node is alive and reporting" from the
        // environmental payload itself, and detect a dead/offline node
        // even if environment data were to stop updating for some reason.
        mqtt_manager_publish_heartbeat(
            &current_sensor_data
        );
    }
}

/**
 * @brief Firmware entry point (called by the ESP-IDF startup code).
 *
 * Overall boot sequence:
 *  1. Initialize NVS flash (erasing and reinitializing it if it's in a bad
 *     state due to a version mismatch or running out of free pages -- this
 *     is the standard ESP-IDF boilerplate pattern for NVS bring-up).
 *  2. Initialize and start the SEN5x sensor (sen5x_init()) BEFORE
 *     networking, so sensor bring-up doesn't depend on/wait for the
 *     network.
 *  3. Bring up the network (network_start()). The result gates everything
 *     related to node identity and MQTT reporting:
 *       - On failure: just logs an error and skips straight past the
 *         identity/MQTT setup block. Node ID is never generated and MQTT is
 *         never started in this case.
 *       - On success:
 *           a. Sync time over SNTP (time_sync_start()).
 *           b. Fetch this node's stored identity/location
 *              (app_wifi_prov_get_node_info()) and copy it into the shared
 *              current_sensor_data struct.
 *           c. Generate this node's unique ID from its MAC address
 *              (generate_node_id()) and log it.
 *           d. Start the MQTT manager (mqtt_manager_start()), passing the
 *              node ID (presumably used to build this node's MQTT topics
 *              and/or client ID). If that succeeds, publish a one-time
 *              node-info message describing this node's identity
 *              (mqtt_manager_publish_node_info()); if it fails, just log an
 *              error and continue booting without MQTT.
 *  4. Start the local web server (start_webserver()) -- this happens
 *     unconditionally, regardless of whether networking/MQTT above
 *     succeeded, so the local status page/API is available even in a
 *     degraded (offline) boot.
 *  5. Launch the recurring sen5x_task() background task that polls the
 *     sensor and publishes readings/heartbeats once per second -- also
 *     started unconditionally.
 *  6. Idle-loop forever, logging a heartbeat message every 10 seconds
 *     (app_main() must not return on ESP-IDF; this keeps the task alive
 *     and doubles as a simple "is the node still alive" log line).
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ENTERED app_main");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    esp_err_t ota_result = ota_manager_init();

    if (ota_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA initialization failed: %s",
                esp_err_to_name(ota_result));
    } else {
        ESP_LOGI(TAG, "OTA manager initialized");
    }

    ESP_LOGI(TAG, "Starting UCM ESP32-P4 environmental node");
   
    ESP_LOGI(TAG, "BEFORE SEN5X");
    sen5x_init();
    ESP_LOGI(TAG, "AFTER SEN5X");


    esp_err_t network_result = network_start();

    if (network_result != ESP_OK) {
        ESP_LOGE(TAG, "Network initialization failed");
    } else {
        time_sync_start();

        char node_name[32] = {0};
        char node_desc[64] = {0};
        float lat = 0.0f;
        float lon = 0.0f;

        app_wifi_prov_get_node_info(
            node_name,
            sizeof(node_name),
            node_desc,
            sizeof(node_desc),
            &lat,
            &lon
        );

        strncpy(current_sensor_data.node_name,
                node_name,
                sizeof(current_sensor_data.node_name) - 1);

        strncpy(current_sensor_data.description,
                node_desc,
                sizeof(current_sensor_data.description) - 1);

        current_sensor_data.latitude = lat;
        current_sensor_data.longitude = lon;

        generate_node_id(
            current_sensor_data.node_id,
            sizeof(current_sensor_data.node_id)
        );

        ESP_LOGI(TAG, "Node ID: %s", current_sensor_data.node_id);

        // Note: MQTT startup/publish is only attempted here, inside the
        // network-success branch, since there is no point trying to connect to a
        // broker without a working network connection.
        if (mqtt_manager_start(current_sensor_data.node_id) == ESP_OK) {
            mqtt_manager_publish_node_info(&current_sensor_data);
        } else {
            ESP_LOGE(TAG, "MQTT startup failed");
        }
    }

    web_server_start(WEB_SERVER_MODE_NORMAL);
    esp_err_t ota_confirm_result = ota_manager_confirm_running_image();

    if (ota_confirm_result != ESP_OK) {
        ESP_LOGE(TAG,
                "OTA firmware confirmation failed: %s",
                esp_err_to_name(ota_confirm_result));
    } else {
        ESP_LOGI(TAG, "OTA firmware confirmed successfully");
    }

    xTaskCreate(
        sen5x_task,
        "sen5x_task",
        4096,
        NULL,
        5,
        NULL
    );

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Network Active...");
    }
}