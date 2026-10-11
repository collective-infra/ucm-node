/**
 * @file web_server.c
 * @brief Local HTTP status dashboard for a UCM sensor node.
 *
 * Starts a small embedded HTTP server (ESP-IDF's esp_http_server) with two
 * routes:
 *   - GET /       Serves a self-contained HTML/CSS/JS dashboard page that
 *                  displays the node's latest readings, polling itself
 *                  every 5 seconds.
 *   - GET /data   Serves the current sensor/node data as JSON; this is
 *                  what the dashboard page's JS polls, but it's also a
 *                  plain machine-readable endpoint on its own (e.g. for
 *                  curl/scripts/other tooling on the local network).
 *
 * This gives anyone on the local network a live view of one specific
 * node's readings by browsing directly to its IP/hostname, independent of
 * MQTT/the broker/any backend dashboard -- useful for on-site debugging,
 * install verification, or simple standalone use without any other
 * project infrastructure running.
 *
 * Data source: reads directly from the shared, global current_sensor_data
 * struct (declared in sen54_data.h) that node_main.c's sen5x_task() writes
 * to once per second -- there is no separate copy or caching layer here,
 * so /data always reflects whatever the most recent write left in that
 * struct (see the note on data_handler() below about the lack of
 * synchronization).
 */

#include "esp_http_server.h"
#include "esp_log.h"
#include "sen54_data.h"   // current_sensor_data -- the shared struct this server reads from

#ifndef FW_COMMIT_HASH
#define FW_COMMIT_HASH "unknown"
#endif
#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

static const char *TAG = "WEB_SERVER";

/**
 * @brief HTTP handler for GET / -- serves the dashboard's HTML page.
 *
 * @param req  Incoming request handle, provided by esp_http_server.
 * @return ESP_OK (httpd_resp_send()'s result isn't checked/propagated).
 *
 * The page itself does not contain any live values -- every field starts
 * as a "--" placeholder <span>, and is filled in client-side by the
 * embedded JavaScript, which:
 *   1. Runs updateData() immediately on load, and again every 5 seconds
 *      (setInterval(updateData, 5000)).
 *   2. Each call fetches GET /data (see data_handler() below), parses it
 *      as JSON, and writes each field into its corresponding element by
 *      ID (e.g. data.temperature -> the #temperature span).
 *   3. Silently logs (console.log) any fetch/parse error rather than
 *      showing an error state in the UI -- e.g. if the node's own /data
 *      endpoint is briefly unavailable, the page will just keep showing
 *      the last successfully fetched values (or "--" if it never
 *      succeeded) without any visible warning to the viewer.
 *
 * NOTE: the JS references data.name (used to set the page's <h1>, falling
 * back to "UCM Sensor Node" if absent via `data.name || 'UCM Sensor
 * Node'`), but data_handler() below does not actually include a "name"
 * field in its JSON output (it sends node_name under other UI fields but
 * not this one) -- so the heading will always fall back to the default
 * text rather than showing the node's configured name. Harmless (the `||`
 * fallback prevents a JS error), but likely not the intended behavior.
 *
 * The entire page is a single hardcoded C string (html[]) built and sent
 * in one shot via httpd_resp_send() with HTTPD_RESP_USE_STRLEN, i.e. no
 * templating -- any dashboard layout/styling changes require editing this
 * string directly and reflashing.
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char html[] =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>UCM Sensor Dashboard</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; margin: 20px; }"
        "h1 { font-size: 28px; }"
        ".card { border: 1px solid #ccc; border-radius: 8px; padding: 15px; margin-bottom: 15px; }"
        ".value { font-size: 24px; font-weight: bold; }"
        ".fw { font-size: 11px; color: #888; margin-top: 20px; }"
        "</style>"
        "</head>"

        "<body>"

        "<h1 id='name'>UCM Sensor Dashboard</h1>"

        "<div class='card'>"
        "<h2>Environment</h2>"
        "<p>Temperature</p>"
        "<div class='value'><span id='temperature'>--</span> C</div>"
        "<p>Humidity</p>"
        "<div class='value'><span id='humidity'>--</span> %</div>"
        "</div>"

        "<div class='card'>"
        "<h2>Air Quality</h2>"
        "<p>PM1.0: <span id='pm1'>--</span> ug/m3</p>"
        "<p>PM2.5: <span id='pm25'>--</span> ug/m3</p>"
        "<p>PM4.0: <span id='pm4'>--</span> ug/m3</p>"
        "<p>PM10: <span id='pm10'>--</span> ug/m3</p>"
        "<p>VOC Index: <span id='voc'>--</span></p>"
        "</div>"

        "<div class='card'>"
        "<h2>Node Information</h2>"
        "<p>ID: <span id='node'>--</span></p>"
        "<p>Location: <span id='location'>--</span></p>"
        "<p>Last Reading: <span id='timestamp'>--</span></p>"
        "</div>"
        "<p class='fw'>Firmware: " FW_COMMIT_HASH " &middot; Built: " FW_BUILD_TIME "</p>"

        "<script>"
        "function updateData() {"
        " fetch('/data')"
        " .then(response => response.json())"
        " .then(data => {"
        " document.getElementById('name').innerHTML = data.name || 'UCM Sensor Node';"
        " document.getElementById('temperature').innerHTML = data.temperature.toFixed(1);"
        " document.getElementById('humidity').innerHTML = data.humidity.toFixed(1);"
        " document.getElementById('pm1').innerHTML = data.pm1.toFixed(1);"
        " document.getElementById('pm25').innerHTML = data.pm25.toFixed(1);"
        " document.getElementById('pm4').innerHTML = data.pm4.toFixed(1);"
        " document.getElementById('pm10').innerHTML = data.pm10.toFixed(1);"
        " document.getElementById('voc').innerHTML = data.voc.toFixed(1);"
        " document.getElementById('node').innerHTML = data.node;"
        " document.getElementById('location').innerHTML = data.lat + ', ' + data.lon;"
        " document.getElementById('timestamp').innerHTML = data.timestamp;"
        " })"
        " .catch(error => console.log(error));"
        "}"

        "updateData();"
        "setInterval(updateData, 5000);"

        "</script>"

        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

/**
 * @brief HTTP handler for GET /data -- serves the node's current sensor
 *        and identity data as a JSON object.
 *
 * @param req  Incoming request handle, provided by esp_http_server.
 * @return ESP_OK (httpd_resp_send()'s result isn't checked/propagated).
 *
 * Reads directly from the shared global current_sensor_data struct (same
 * one node_main.c's sen5x_task() writes to once per second, and the same
 * one mqtt_manager.c's publish functions read from) and formats it as a
 * single flat JSON object mirroring the same fields published over MQTT
 * (node/name/description/lat/lon/timestamp + the PM/temperature/humidity/
 * VOC readings).
 *
 * NOTE: current_sensor_data's string fields (node_name, description,
 * timestamp, node_id) are inserted into this JSON via plain %s with no
 * escaping. If any of those ever contained a double-quote or backslash
 * character (e.g. a node name/description set via provisioning), the
 * resulting JSON would be malformed and could fail to parse in the
 * dashboard's fetch(...).json() call. In practice this depends on
 * whatever validates/sanitizes those fields at provisioning time
 * elsewhere in the project.
 *
 * NOTE: as with the sensor task in node_main.c, there is no mutex/lock
 * around reads of current_sensor_data here, so a request handled while
 * sen5x_task() is mid-update could theoretically read a partially updated
 * struct (e.g. some fields from an old reading, some from a new one).
 */
static esp_err_t data_handler(httpd_req_t *req)
{
    char json[512];
    snprintf(json,
             sizeof(json),
             "{"
             "\"node\":\"%s\","
             "\"name\":\"%s\","
             "\"description\":\"%s\","
             "\"lat\":%.5f,"
             "\"lon\":%.5f,"
             "\"timestamp\":\"%s\","
             "\"pm1\":%.1f,"
             "\"pm25\":%.1f,"
             "\"pm4\":%.1f,"
             "\"pm10\":%.1f,"
             "\"temperature\":%.1f,"
             "\"humidity\":%.1f,"
             "\"voc\":%.1f"
             "}",
             current_sensor_data.node_id,
             current_sensor_data.node_name,
             current_sensor_data.description,
             current_sensor_data.latitude,
             current_sensor_data.longitude,
             current_sensor_data.timestamp,
             current_sensor_data.pm1,
             current_sensor_data.pm25,
             current_sensor_data.pm4,
             current_sensor_data.pm10,
             current_sensor_data.temperature,
             current_sensor_data.humidity,
             current_sensor_data.voc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Starts the local HTTP server and registers the / and /data
 *        routes. Called once from node_main.c's app_main(),
 *        unconditionally (regardless of whether networking/MQTT setup
 *        succeeded), so this dashboard is available whenever the node has
 *        any IP connectivity at all.
 *
 * Uses esp_http_server's default configuration (HTTPD_DEFAULT_CONFIG()) --
 * i.e. default port (80), default max URI handlers, default stack size,
 * etc. -- rather than customizing any server settings.
 *
 * NOTE: if httpd_start() fails, this function does nothing further (no
 * else branch) -- no error is logged, and the caller (app_main()) has no
 * way to know the web server didn't come up, since this function has a
 * void return type. If the dashboard is unexpectedly unreachable on a
 * given node, this silent-failure path is worth checking first.
 *
 * NOTE: the local `server` handle is not stored anywhere outside this
 * function (e.g. in a static/module-level variable), so there is
 * currently no way for any other code to later call httpd_stop() on this
 * server -- once started, it runs for the lifetime of the firmware.
 */
void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t data = {
            .uri = "/data",
            .method = HTTP_GET,
            .handler = data_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &data);

        ESP_LOGI(TAG, "Web server started");
    }
}