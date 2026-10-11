/**
 * @file mqtt_manager.c
 * @brief MQTT connectivity layer for a UCM sensor node -- owns the MQTT
 *        client lifecycle, topic naming, and all outbound publishes
 *        (node info, environment readings, heartbeats), plus a small
 *        inbound command channel (currently just remote re-provisioning).
 *
 * This is the module that node_main.c's app_main()/sen5x_task() delegate
 * to (mqtt_manager_start(), mqtt_manager_publish_node_info(),
 * mqtt_manager_publish_environment(), mqtt_manager_publish_heartbeat())
 * instead of talking to the esp-mqtt client directly, so all
 * broker/topic/QoS/retain decisions live in one place.
 *
 * TOPIC LAYOUT (publish side, node-specific, all namespaced under
 * "ucm/node-<node_id>/..."):
 *   - ucm/node-<id>/status       "online"/"offline" presence, retained
 *                                 (see publish_online_status() and the LWT
 *                                 setup in mqtt_manager_start()).
 *   - ucm/node-<id>/info         Static identity (name/description/lat/lon),
 *                                 retained, published once via
 *                                 mqtt_manager_publish_node_info().
 *   - ucm/node-<id>/environment  Live sensor readings, NOT retained,
 *                                 published every ~1s via
 *                                 mqtt_manager_publish_environment().
 *   - ucm/node-<id>/heartbeat    Uptime/free-heap/Wi-Fi RSSI health data,
 *                                 NOT retained, published alongside every
 *                                 environment reading via
 *                                 mqtt_manager_publish_heartbeat().
 *
 * TOPIC LAYOUT (subscribe side): see mqtt_event_handler()'s
 * MQTT_EVENT_CONNECTED case -- subscribes to "ucm/node/+/command". Note
 * this uses a *slash* between "node" and the wildcard segment, which does
 * not match the "node-<id>" (hyphenated, no wildcard) pattern used
 * everywhere else in this file for publishing. This is flagged in detail
 * at that subscription site below.
 *
 * BROKER DISCOVERY: connects to "mqtt://nyit-ucm.nycmesh.net", an mDNS hostname
 * rather than a hardcoded IP (contrast with the earlier draft firmware's
 * hardcoded broker IP) -- this assumes mDNS/Bonjour resolution is enabled
 * and working on the network the node is deployed on.
 */

#include <stdio.h>
#include <string.h>

#include "mqtt_manager.h"
#include "ota_manager.h"
#include "secrets.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   // esp_get_free_heap_size(), used in the heartbeat payload
#include "esp_wifi.h"        // esp_wifi_sta_get_ap_info(), used to read Wi-Fi RSSI for the heartbeat
#include "esp_timer.h"       // esp_timer_get_time(), used to compute uptime for the heartbeat
#include "app_wifi_prov.h"   // app_wifi_prov_reset_credentials(), used by the remote "provision" command

#ifndef FW_COMMIT_HASH
#define FW_COMMIT_HASH "unknown"
#endif
#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

// Broker address as an mDNS hostname (".local") rather than a static IP,
// so this doesn't need to be reconfigured if the broker's IP changes --
// relies on mDNS resolution being available on the network.

#define MQTT_BROKER_URI "mqtt://nyit-ucm.nycmesh.net"

static const char *TAG = "MQTT_MANAGER";

// Single MQTT client instance for this node; NULL until
// mqtt_manager_start() has run. All publish functions below guard on this
// being non-NULL so they safely no-op (return ESP_FAIL) if called before
// start or if start was never called/succeeded.
static esp_mqtt_client_handle_t mqtt_client = NULL;

// Module-level state populated once in mqtt_manager_start() and reused by
// publish_online_status() and the LWT (Last Will and Testament) config;
// kept as fixed-size buffers rather than allocated per-call since they're
// needed for the lifetime of the MQTT connection (the LWT payload/topic
// pointers handed to esp-mqtt at connect time must remain valid for as
// long as the client is running).
static char s_node_id[32];
static char s_username[40];
static char s_status_topic[64];
static char s_lwt_payload[96];


/**
 * @brief Publishes a retained "online" presence message for this node.
 *
 * Called once the MQTT connection is established (see
 * MQTT_EVENT_CONNECTED below). This is the counterpart to the LWT
 * ("offline") message configured in mqtt_manager_start(): together they
 * let subscribers always know, via the retained ucm/node-<id>/status
 * topic, whether this node is currently connected -- "online" while
 * connected (set here), automatically flipped to "offline" by the broker
 * itself if the client disconnects ungracefully (network drop, crash,
 * power loss) without this node getting a chance to say so.
 */
static void publish_online_status(void)
{
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"node\":\"%s\",\"status\":\"online\","
             "\"fw_commit_hash\":\"%s\",\"fw_build_time\":\"%s\"}",
             s_node_id, FW_COMMIT_HASH, FW_BUILD_TIME);

    esp_mqtt_client_publish(mqtt_client, s_status_topic, payload,
                             0, /* qos */ 1, /* retain */ 1);
}


/**
 * @brief esp-mqtt event callback, registered with the client in
 *        mqtt_manager_start(). Handles connection lifecycle and incoming
 *        command messages.
 *
 * @param handler_args  Unused (registered with NULL as the handler arg).
 * @param base           Unused (event base, always the MQTT event base
 *                        here since this handler is only registered for
 *                        MQTT client events).
 * @param event_id       Which MQTT event fired; cast to
 *                        esp_mqtt_event_id_t and switched on below.
 * @param event_data     Event payload, cast to esp_mqtt_event_handle_t.
 *
 * Handled events:
 *   - MQTT_EVENT_CONNECTED: subscribes to the command topic and announces
 *     this node as online (see subscribe/publish notes below).
 *   - MQTT_EVENT_DATA: a message arrived on a subscribed topic; currently
 *     the only command recognized is a hardcoded "provision" request that
 *     triggers a factory-reset-and-reboot into provisioning mode.
 *   - All other event IDs (disconnect, subscribed, published, error, etc.)
 *     are ignored (default: break) -- e.g. there's no explicit handling of
 *     MQTT_EVENT_DISCONNECTED here; reconnection is left entirely to
 *     esp-mqtt's built-in auto-reconnect behavior.
 */


static void ota_update_task(void *pvParameters)
{
 /*   const char *firmware_url =
  *      "http://ucm-hub.local/ota/ucm-node.bin";
  */ 
    const char *firmware_url =
        "http://nyit-ucm.nycmesh.net/ota/ucm-node.bin";
    
    ESP_LOGI(TAG, "OTA task started");

    esp_err_t ota_result =
        ota_manager_update(firmware_url);

    if (ota_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "OTA task failed: %s",
                 esp_err_to_name(ota_result));
    }

    vTaskDelete(NULL);
}
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");

        char topic[64];
        // NOTE: this subscribes to "ucm/node/+/command" -- with a SLASH
        // between "node" and the single-level wildcard "+". Every publish
        // topic elsewhere in this file uses a hyphen instead
        // ("ucm/node-<id>/..."), with no wildcard segment. As written,
        // this subscription will only match topics literally shaped like
        // "ucm/node/<anything>/command" -- it will NOT match a topic like
        // "ucm/node-AB12CD/command" that mirrors this node's own
        // publish-side naming convention. If commands are meant to be
        // addressed per-node the same way status/info/environment/
        // heartbeat topics are, this subscription pattern likely needs to
        // change to match (e.g. "ucm/node-+/command" or
        // "ucm/node-%s/command" with s_node_id substituted in, depending
        // on whether commands should be broadcast to all nodes or targeted
        // at this one). Left as-is here per instructions not to alter the
        // code; flagging for whoever picks this up next.

        snprintf(topic, sizeof(topic),
         "ucm/node-%s/command",
         s_node_id);

        ESP_LOGI(TAG, "Subscribe topic='%s' length=%d", topic, strlen(topic));

        esp_mqtt_client_subscribe(mqtt_client, topic, 1);

        ESP_LOGI(TAG, "Subscribed to %s", topic);

        publish_online_status();
        break;


case MQTT_EVENT_DATA:

    ESP_LOGI(TAG, "MQTT command topic: %.*s",
             event->topic_len, event->topic);

    ESP_LOGI(TAG, "MQTT command data: %.*s",
             event->data_len, event->data);


    /*
     * ---------------------------------------------------------
     * PROVISION COMMAND
     * ---------------------------------------------------------
     */

    static const char *provision_command =
        "{\"command\":\"provision\"}";

    size_t provision_command_len =
        strlen(provision_command);

    if (event->data_len == provision_command_len &&
        memcmp(event->data,
               provision_command,
               provision_command_len) == 0) {

        ESP_LOGW(TAG, "Provision command received");

        app_wifi_prov_reset_credentials();

        vTaskDelay(pdMS_TO_TICKS(1000));

        esp_restart();
    }


    /*
     * ---------------------------------------------------------
     * OTA COMMAND
     * ---------------------------------------------------------
     */

   static const char *ota_command =
    "{\"command\":\"ota\"}";

size_t ota_command_len =
    strlen(ota_command);

if (event->data_len == ota_command_len &&
    memcmp(event->data,
           ota_command,
           ota_command_len) == 0) {

    ESP_LOGW(TAG, "OTA command received");

    BaseType_t task_result =
        xTaskCreate(
            ota_update_task,
            "ota_update",
            8192,
            NULL,
            5,
            NULL
        );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
    }
}

    break;


    default:
        break;
    }
}


/**
 * @brief Initializes node identity/topic/LWT state, then creates,
 *        registers callbacks for, and starts the MQTT client.
 *
 * @param node_id  This node's unique ID string (see generate_node_id() in
 *                 node_main.c), copied into module-local storage.
 * @return ESP_OK once the client has been created and esp_mqtt_client_start()
 *         has been called. Note: this reflects that the *start* call
 *         succeeded, not that a broker connection has actually been
 *         established yet -- esp-mqtt connects asynchronously, and actual
 *         connection success is only known once MQTT_EVENT_CONNECTED fires
 *         in mqtt_event_handler(). ESP_ERROR_CHECK() is used on the
 *         registration/start calls, so a genuine failure there aborts
 *         firmware boot rather than returning an error code from this
 *         function.
 *
 * Sets up:
 *   - s_node_id / s_status_topic / s_lwt_payload: node-specific state
 *     used both here and later by publish_online_status().
 *   - The MQTT Last Will and Testament (LWT): tells the broker to
 *     automatically publish an "offline" status message (retained) to
 *     this node's status topic if the client disconnects without cleanly
 *     signing off -- see publish_online_status() for the "online"
 *     counterpart. This is how downstream consumers detect a node going
 *     dark (crash, power loss, network failure) without needing their own
 *     timeout logic.
 *   - Registers mqtt_event_handler() for ESP_EVENT_ANY_ID, i.e. all MQTT
 *     client events, not just the ones actually handled in the switch
 *     statement there.
 */
esp_err_t mqtt_manager_start(const char *node_id)
{
    strncpy(s_node_id, node_id, sizeof(s_node_id) - 1);
    s_node_id[sizeof(s_node_id) - 1] = '\0';

    snprintf(s_username, sizeof(s_username), "node-%s", s_node_id);

    snprintf(s_status_topic, sizeof(s_status_topic), "ucm/node-%s/status", s_node_id);
    snprintf(s_lwt_payload, sizeof(s_lwt_payload),
             "{\"node\":\"%s\",\"status\":\"offline\"}", s_node_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = s_username,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will.topic = s_status_topic,
        .session.last_will.msg = s_lwt_payload,
        .session.last_will.msg_len = strlen(s_lwt_payload),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(mqtt_client)
    );

    ESP_LOGI(TAG, "MQTT started");

    return ESP_OK;
}


/**
 * @brief Publishes this node's static identity (ID, name, description,
 *        GPS coordinates) as a retained MQTT message.
 *
 * @param data  Pointer to the node's current sensor/identity data struct
 *              (only the identity fields -- node_id/node_name/description/
 *              latitude/longitude -- are used here).
 * @return ESP_FAIL if the MQTT client hasn't been started yet
 *         (mqtt_client == NULL); ESP_OK otherwise (this does not confirm
 *         the publish was actually delivered/acked, only that it was
 *         handed to the client).
 *
 * Published retained (see esp_mqtt_client_publish()'s last argument = 1)
 * to ucm/node-<id>/info, so new subscribers immediately learn a node's
 * identity/location without this node needing to resend it.
 */
esp_err_t mqtt_manager_publish_node_info(sen54_data_t *data)
{
    if (!mqtt_client) {
        return ESP_FAIL;
    }

    char payload[384];

    snprintf(payload, sizeof(payload),
             "{"
             "\"node\":\"%s\","
             "\"name\":\"%s\","
             "\"description\":\"%s\","
             "\"lat\":%.5f,"
             "\"lon\":%.5f,"
             "\"fw_commit_hash\":\"%s\","
             "\"fw_build_time\":\"%s\""
             "}",
             data->node_id,
             data->node_name,
             data->description,
             data->latitude,
             data->longitude,
             FW_COMMIT_HASH,
             FW_BUILD_TIME
            );

    char topic[64];
    snprintf(topic, sizeof(topic), "ucm/node-%s/info", data->node_id);

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    ESP_LOGI(TAG, "Published node info: %s", payload);

    return ESP_OK;
}


/**
 * @brief Publishes a live environmental sensor reading (PM1/2.5/4/10,
 *        temperature, humidity, VOC) as a non-retained MQTT message.
 *
 * @param data  Pointer to the node's current sensor data struct; all
 *              fields here are expected to already be populated/scaled
 *              (see sen5x_task() in node_main.c, which converts the raw
 *              sensor integer readings to physical units before calling
 *              this).
 * @return ESP_FAIL if the MQTT client hasn't been started yet; ESP_OK
 *         otherwise.
 *
 * Published NOT retained (last argument = 0) to
 * ucm/node-<id>/environment, since a stale reading should never be handed
 * to a new subscriber as if it were current -- unlike node info or
 * status, which describe things that don't (or rarely) change.
 */
esp_err_t mqtt_manager_publish_environment(sen54_data_t *data)
{
    if (!mqtt_client) {
        return ESP_FAIL;
    }

    char payload[256];

    snprintf(payload, sizeof(payload),
             "{"
             "\"node\":\"%s\","
             "\"timestamp\":\"%s\","
             "\"pm1\":%.1f,"
             "\"pm25\":%.1f,"
             "\"pm4\":%.1f,"
             "\"pm10\":%.1f,"
             "\"temperature\":%.1f,"
             "\"humidity\":%.1f,"
             "\"voc\":%.1f"
             "}",
             data->node_id,
             data->timestamp,
             data->pm1,
             data->pm25,
             data->pm4,
             data->pm10,
             data->temperature,
             data->humidity,
             data->voc);

    char topic[64];
    snprintf(topic, sizeof(topic), "ucm/node-%s/environment", data->node_id);

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);

    return ESP_OK;
}


/**
 * @brief Publishes a node health/liveness message: uptime, free heap, and
 *        Wi-Fi signal strength.
 *
 * @param data  Pointer to the node's current sensor data struct; only
 *              node_id and timestamp are used from it here (the rest of
 *              the payload is computed fresh each call, below).
 * @return ESP_FAIL if the MQTT client hasn't been started yet; ESP_OK
 *         otherwise.
 *
 * Computes, at call time:
 *   - free_heap: current free heap size (esp_get_free_heap_size()) --
 *     useful for spotting memory leaks in the field over time.
 *   - uptime_s: seconds since boot, derived from esp_timer_get_time()
 *     (which returns microseconds since boot) divided down to whole
 *     seconds.
 *   - rssi: current Wi-Fi access point signal strength, read via
 *     esp_wifi_sta_get_ap_info(). If that call fails (e.g. because this
 *     node is on Ethernet/PoE rather than Wi-Fi station mode, or is
 *     between reconnects), rssi silently stays at its initialized value
 *     of 0 rather than surfacing the failure -- a 0 dBm reading in the
 *     published payload should be interpreted with that in mind rather
 *     than taken as a literal signal strength.
 *
 * Published NOT retained (last argument = 0) to
 * ucm/node-<id>/heartbeat, for the same reason as environment readings --
 * a stale heartbeat shouldn't look like a live one to a new subscriber.
 * Called once per second alongside mqtt_manager_publish_environment() (see
 * sen5x_task() in node_main.c).
 */
esp_err_t mqtt_manager_publish_heartbeat(sen54_data_t *data)
{
    if (!mqtt_client) {
        return ESP_FAIL;
    }

    uint32_t free_heap = esp_get_free_heap_size();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    int8_t rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    char payload[256];

    snprintf(payload, sizeof(payload),
             "{"
             "\"node\":\"%s\","
             "\"timestamp\":\"%s\","
             "\"uptime_s\":%lld,"
             "\"free_heap\":%lu,"
             "\"rssi\":%d"
             "}",
             data->node_id,
             data->timestamp,
             uptime_s,
             free_heap,
             rssi);

    char topic[64];
    snprintf(topic, sizeof(topic), "ucm/node-%s/heartbeat", data->node_id);

    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);

    ESP_LOGI(TAG, "Heartbeat: %s", payload);

    return ESP_OK;
}
