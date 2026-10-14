/**
 * @file app_wifi_prov.c
 * @brief Wi-Fi provisioning, credential storage, and stored node identity
 *        (name/description/lat/lon) for a UCM sensor node.
 *
 * This module is the Wi-Fi fallback path referenced by network_manager.c
 * (called as app_wifi_prov_start() when Ethernet doesn't come up in
 * time), and it's also where a node's user-facing identity fields --
 * name, description, latitude, longitude -- are captured, persisted, and
 * later handed back out via app_wifi_prov_get_node_info() (used by
 * node_main.c's app_main() to populate current_sensor_data).
 *
 * HIGH-LEVEL FLOW
 * -----------------
 * 1. app_wifi_prov_start() checks NVS for previously saved Wi-Fi
 *    credentials.
 * 2a. If none are found (first boot, or after a reset -- see
 *     app_wifi_prov_reset_credentials()): starts a local open Wi-Fi
 *     access point ("UCM-SENSOR-SETUP") and a captive-portal-style HTTP
 *     server (start_setup_server_and_ap()) serving a setup form at
 *     http://192.168.4.1. The installer connects their phone to this AP,
 *     fills in the target Wi-Fi SSID/password plus optional node
 *     name/description/GPS coordinates, and submits. The form handler
 *     (save_post_handler()) saves everything to NVS and reboots the
 *     device. This function never returns in this branch -- the reboot
 *     restarts app_main()'s whole boot sequence from scratch.
 * 2b. If credentials ARE found: attempts to join that Wi-Fi network as a
 *     station (STA), retrying up to MAXIMUM_RETRY times on failure. If it
 *     never connects, credentials are wiped and the device falls back to
 *     setup mode (2a) -- e.g. useful if a saved password is stale/wrong
 *     and the node would otherwise be stuck retrying it forever with no
 *     way for an installer to intervene except physically resetting it.
 *     If it DOES connect, this function also starts mDNS advertising the
 *     node's hostname/description on the local network before returning
 *     ESP_OK.
 *
 * STORAGE: all persisted fields (SSID, password, node name/description,
 * lat/lon-as-strings) live in a single NVS namespace ("wifi_cfg"). Note
 * that Wi-Fi passwords are stored here as plain strings with no
 * additional encryption applied by this code -- if NVS-at-rest encryption
 * isn't separately enabled at the project/partition level, credentials
 * are recoverable by anyone with physical/flash access to the device.
 *
 * SECURITY NOTE ON SETUP MODE: the setup AP (SETUP_AP_SSID) is created
 * with WIFI_AUTH_OPEN (no password) and the setup form is served over
 * plain HTTP, so both the portal itself and the Wi-Fi credentials
 * submitted through it are unencrypted and accessible to anyone in radio
 * range while a node is in setup mode. This is a reasonable tradeoff for
 * ease of onboarding but worth knowing if deploying in a
 * security-sensitive environment.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "app_wifi_prov.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "web_server.h"
#include "ota_manager.h"

static const char *TAG = "app_wifi_prov";

#define SETUP_AP_SSID   "UCM-SENSOR-SETUP"
#define NVS_NAMESPACE   "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_NAME    "node_name"
#define NVS_KEY_DESC    "node_desc"
#define NVS_KEY_LAT     "node_lat"
#define NVS_KEY_LON     "node_lon"
#define MAXIMUM_RETRY   5

// FreeRTOS event group used to signal STA connection outcome
// (WIFI_CONNECTED_BIT / WIFI_FAIL_BIT) from the async Wi-Fi/IP event
// handler back to the blocking wait in app_wifi_prov_start().
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// Cached after a successful load/save so app_wifi_prov_get_node_info()
// can hand it back without re-touching NVS.
static char s_node_name[32] = {0};
static char s_node_desc[64] = {0};
static float s_node_lat = 0.0f;
static float s_node_lon = 0.0f;

// Setup form: Wi-Fi credentials + node identity/location fields.
static const char SETUP_FORM_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>Sensor Setup</title>"
"<style>"
"body{font-family:sans-serif;background:#111;color:#eee;padding:24px;}"
"h1{font-size:1.2em;color:#8ab4f8;}"
"h2{font-size:1em;color:#8ab4f8;margin-top:28px;}"
"label{display:block;margin-top:16px;font-size:0.9em;color:#aaa;}"
"input{width:100%;padding:10px;margin-top:4px;border-radius:8px;"
"border:none;font-size:1em;box-sizing:border-box;}"
"button{margin-top:28px;width:100%;padding:12px;border-radius:8px;"
"border:none;background:#8ab4f8;color:#111;font-size:1em;font-weight:600;}"
"</style></head><body>"
"<h1>Set up this sensor</h1>"
"<form method='POST' action='/save'>"
"<h2>Wi-Fi</h2>"
"<label>Network name (SSID)</label>"
"<input name='ssid' required>"
"<label>Password</label>"
"<input name='pass' type='password'>"
"<h2>Sensor info (optional)</h2>"
"<label>Node name (e.g. Lobby, Warehouse-2)</label>"
"<input name='name' maxlength='31'>"
"<label>Description</label>"
"<input name='desc' maxlength='63'>"
"<label>Latitude</label>"
"<input name='lat' type='text' inputmode='text' placeholder='e.g. 40.7128'>"
"<label>Longitude</label>"
"<input name='lon' type='text' inputmode='text' placeholder='e.g. -74.0060'>"
"<button type='submit'>Save &amp; Connect</button>"
"</form></body></html>";

static const char SAVED_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"</head><body style='font-family:sans-serif;background:#111;color:#eee;"
"padding:24px;'><h1 style='color:#8ab4f8;'>Saved!</h1>"
"<p>The sensor is rebooting and connecting to your Wi-Fi. "
"You can close this page and reconnect your phone to your normal Wi-Fi.</p>"
"</body></html>";

/**
 * @brief HTTP GET handler for "/" while in setup mode -- serves the
 *        static setup form (SETUP_FORM_HTML).
 */
static esp_err_t form_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETUP_FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief Minimal application/x-www-form-urlencoded decoder: converts
 *        "%XX" hex escapes and "+" (space) escapes into their literal
 *        characters, copying everything else through unchanged.
 *
 * @param dst       Destination buffer for the decoded string.
 * @param src       Source (still-encoded) string, NUL-terminated.
 * @param dst_size  Size of the destination buffer; decoding stops early
 *                  (truncating) if it would be exceeded.
 *
 * This is a deliberately minimal decoder ("good enough for our text
 * fields" per the original inline comment) -- it doesn't validate that
 * the two characters after '%' are actually valid hex digits, for
 * example, it just arithmetically maps 'A'-'F'/'a'-'f'/'0'-'9' the same
 * way regardless. Adequate for decoding a browser's own form-encoded
 * output, but not a general-purpose/hardened URL decoder.
 */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di + 1 < dst_size) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = src[1] >= 'A' ? (src[1] & ~0x20) - 'A' + 10 : src[1] - '0';
            int lo = src[2] >= 'A' ? (src[2] & ~0x20) - 'A' + 10 : src[2] - '0';
            dst[di++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/**
 * @brief Extracts and URL-decodes a single "key=value" field from a raw
 *        application/x-www-form-urlencoded request body.
 *
 * @param body      Full decoded... actually still-encoded raw POST body
 *                  (NUL-terminated).
 * @param key       Field name to look for (e.g. "ssid").
 * @param out       Destination buffer for the decoded value.
 * @param out_size  Size of the destination buffer.
 * @return true if the key was found in the body (out is then populated,
 *         possibly with an empty string if the field had no value);
 *         false if the key wasn't found at all (out is set to an empty
 *         string in that case too).
 *
 * Implementation notes / limitations:
 *   - Uses a simple strstr(body, "key=") search rather than parsing the
 *     body into discrete key/value pairs first. This means a field name
 *     that happens to appear as a *substring* inside another field's
 *     *value* earlier in the body could, in principle, be matched
 *     incorrectly -- not a concern for the fixed, known field set this
 *     form actually submits (ssid/pass/name/desc/lat/lon), but worth
 *     knowing if more fields are added later with the potential for one
 *     field name to be a substring of another value.
 *   - The raw (still-encoded) value is first copied into a fixed 128-byte
 *     stack buffer (raw[128]) before being decoded into `out` -- any
 *     value longer than 127 encoded characters is silently truncated at
 *     that point, independent of out_size.
 */
static bool extract_field(const char *body, const char *key, char *out, size_t out_size)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *found = strstr(body, search);
    if (!found) {
        out[0] = '\0';
        return false;
    }
    found += strlen(search);
    const char *end = strchr(found, '&');
    size_t raw_len = end ? (size_t)(end - found) : strlen(found);
    char raw[128];
    if (raw_len >= sizeof(raw)) {
        raw_len = sizeof(raw) - 1;
    }
    memcpy(raw, found, raw_len);
    raw[raw_len] = '\0';
    url_decode(out, raw, out_size);
    return true;
}

/**
 * @brief HTTP POST handler for "/save" while in setup mode -- reads the
 *        submitted form body, extracts all fields, persists them to NVS,
 *        serves a "Saved!" confirmation page, then reboots the device.
 *
 * @param req  Incoming request handle.
 * @return In practice never returns to a meaningful caller state -- the
 *         function calls esp_restart() near the end, which does not
 *         return; the final `return ESP_OK;` is unreachable dead code
 *         left in only to satisfy the handler's declared function
 *         signature.
 *
 * Steps:
 *  1. Reads the full POST body into a fixed 512-byte stack buffer
 *     (body[512]), looping on httpd_req_recv() until content_len bytes
 *     have been received. If content_len is too large to fit
 *     (>= sizeof(body)) or a read ever fails, responds 500 and bails out
 *     without touching NVS or rebooting.
 *  2. Extracts each of the six known fields (ssid, pass, name, desc, lat,
 *     lon) via extract_field()/url_decode() into local stack buffers.
 *  3. Opens the "wifi_cfg" NVS namespace read-write and writes all six
 *     values as strings (lat/lon are stored as their raw decimal text,
 *     defaulting to "0" if the field was left empty -- NVS has no native
 *     float storage type here, so these are parsed back to float on load
 *     via strtof(), see app_wifi_prov_start()). Every NVS call here is
 *     ESP_ERROR_CHECK'd, so any storage failure aborts firmware
 *     execution outright rather than returning an HTTP error to the
 *     submitter.
 *  4. Serves SAVED_HTML back to the browser.
 *  5. Waits 2 seconds (presumably to give the HTTP response time to
 *     actually flush out to the client before the device drops off Wi-Fi)
 *     and then reboots via esp_restart(). The reboot re-runs the entire
 *     firmware boot sequence from node_main.c's app_main(), which will
 *     find the newly saved credentials and attempt to join the target
 *     network this time.
 *
 * NOTE: this always persists ALL six NVS keys, including name/desc/lat/lon
 * even if those optional fields were left blank in the form -- in that
 * case name/desc are saved as empty strings and lat/lon as "0", so a
 * resubmission with blank identity fields will overwrite any previously
 * saved values for them (there's no "only update fields that were
 * provided" merge behavior).
 */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = 0;
    int remaining = req->content_len;
    if (remaining >= (int)sizeof(body)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + total, remaining);
        if (received <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        total += received;
        remaining -= received;
    }
    body[total] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    char name[32] = {0};
    char desc[64] = {0};
    char lat_str[32] = {0};
    char lon_str[32] = {0};

    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));
    extract_field(body, "name", name, sizeof(name));
    extract_field(body, "desc", desc, sizeof(desc));
    extract_field(body, "lat", lat_str, sizeof(lat_str));
    extract_field(body, "lon", lon_str, sizeof(lon_str));

    ESP_LOGI(TAG, "Received setup form: SSID '%s', name '%s'", ssid, name);

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_PASS, pass));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_NAME, name));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_DESC, desc));
    // Stored as strings (NVS has no native float type); parsed back on load.
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_LAT, lat_str[0] ? lat_str : "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_LON, lon_str[0] ? lon_str : "0"));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Credentials saved, rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK; // unreachable
}

esp_err_t app_wifi_prov_register_http(httpd_handle_t server)
{
    httpd_uri_t form_uri = { .uri = "/setup", .method = HTTP_GET,  .handler = form_get_handler };
    httpd_uri_t save_uri = { .uri = "/save",  .method = HTTP_POST, .handler = save_post_handler };
    esp_err_t err = httpd_register_uri_handler(server, &form_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &save_uri);
    }
    return err;
}

/**
 * @brief Starts the local open Wi-Fi setup access point and the
 *        captive-portal-style HTTP server serving the setup form, then
 *        blocks forever.
 *
 * Never returns (ends in an infinite vTaskDelay loop) -- this is by
 * design: it's only called from paths in app_wifi_prov_start() where
 * there's nothing else useful for the calling task to do except wait for
 * the installer to submit the form (which reboots the device via
 * save_post_handler(), restarting the whole boot sequence).
 *
 * Steps:
 *  1. esp_netif_create_default_wifi_ap() -- creates the default network
 *     interface for AP (access point) mode.
 *  2. esp_wifi_init() with default config, then configures and starts the
 *     Wi-Fi driver in AP mode with:
 *       - SSID: SETUP_AP_SSID ("UCM-SENSOR-SETUP")
 *       - channel 1, WIFI_AUTH_OPEN (no password -- see the security note
 *         in this file's header comment)
 *       - max_connection = 2 (at most 2 clients can join the setup AP at
 *         once)
 *  3. Starts an HTTP server (default config) with two routes:
 *       - GET  /      -> form_get_handler()  (serves the setup form)
 *       - POST /save  -> save_post_handler() (saves + reboots)
 *  4. Logs how to connect (http://192.168.4.1, the standard default IP
 *     ESP-IDF's AP-mode DHCP server hands out to itself) and then idles
 *     forever, since the HTTP server itself runs in its own task(s) and
 *     needs no further driving from here.
 */
static void start_setup_server_and_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 2,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP started: %s (open network)", SETUP_AP_SSID);
    ESP_LOGI(TAG, "Connect your phone to it, then open http://192.168.4.1");

    ESP_ERROR_CHECK(web_server_start(WEB_SERVER_MODE_SETUP) ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Setup web server started");
    // Setup mode never reaches app_main()'s confirm call. Confirm here so a
    // freshly flashed image isn't rolled back on the next reboot.
    ota_manager_confirm_running_image();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/**
 * @brief ESP-IDF event handler covering Wi-Fi STA connection lifecycle,
 *        registered (as instance handlers) in app_wifi_prov_start().
 *
 * @param arg         Unused (registered with NULL).
 * @param event_base  WIFI_EVENT or IP_EVENT.
 * @param event_id     Specific event within that base.
 * @param event_data   Event-specific payload; only used for
 *                      IP_EVENT_STA_GOT_IP (cast to ip_event_got_ip_t*).
 *
 * Handled cases:
 *   - WIFI_EVENT_STA_START: fires once the Wi-Fi driver has started in
 *     station mode; immediately kicks off a connection attempt via
 *     esp_wifi_connect().
 *   - WIFI_EVENT_STA_DISCONNECTED: fires on any disconnect (including an
 *     initial failed connection attempt). Retries up to MAXIMUM_RETRY (5)
 *     times via esp_wifi_connect() + incrementing s_retry_num; once that
 *     limit is reached, sets WIFI_FAIL_BIT on the event group, waking up
 *     the blocking xEventGroupWaitBits() call in app_wifi_prov_start().
 *   - IP_EVENT_STA_GOT_IP: fires once DHCP succeeds after a connection.
 *     Logs the assigned IP, resets s_retry_num back to 0 (so a *future*
 *     disconnect gets its own fresh set of retries rather than being
 *     immediately treated as exhausted), and sets WIFI_CONNECTED_BIT.
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to AP...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Erases ALL saved data in the "wifi_cfg" NVS namespace --
 *        Wi-Fi SSID/password AND the node's name/description/lat/lon --
 *        so the device falls back into setup mode on next boot.
 *
 * @return ESP_OK on success; whatever nvs_open() returned if opening the
 *         namespace failed (nvs_erase_all()/nvs_commit() results are not
 *         individually checked).
 *
 * Called from two places in this project: automatically by
 * app_wifi_prov_start() if it exhausts its Wi-Fi connection retries (see
 * below), and remotely, on demand, by mqtt_manager.c's
 * mqtt_event_handler() in response to a {"command":"provision"} MQTT
 * message.
 *
 * NOTE: because this erases the *entire* NVS namespace rather than just
 * the SSID/password keys, a remote "provision" command (or a failed
 * reconnect) also wipes the node's saved name, description, and GPS
 * coordinates -- not just its Wi-Fi credentials. Anyone triggering this
 * (intentionally or via repeated connection failures) should expect to
 * re-enter the node's identity/location info again during the next setup
 * flow, not just its Wi-Fi password.
 */
esp_err_t app_wifi_prov_reset_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

/**
 * @brief Returns this node's cached identity/location fields (name,
 *        description, latitude, longitude) as loaded from NVS during
 *        app_wifi_prov_start().
 *
 * @param name       Destination buffer for the node name (may be NULL to
 *                    skip).
 * @param name_size  Size of `name`.
 * @param desc       Destination buffer for the description (may be NULL
 *                    to skip).
 * @param desc_size  Size of `desc`.
 * @param lat        Destination for latitude (may be NULL to skip).
 * @param lon        Destination for longitude (may be NULL to skip).
 *
 * Called once by node_main.c's app_main() (after a successful
 * network_start()) to populate the shared current_sensor_data struct.
 * Reads from the module-level s_node_name/s_node_desc/s_node_lat/
 * s_node_lon cache populated in app_wifi_prov_start() -- does not
 * re-touch NVS itself, so this is only meaningful to call after
 * app_wifi_prov_start() has already run (and loaded saved values, if
 * any) earlier in boot.
 */
void app_wifi_prov_get_node_info(char *name, size_t name_size,
                                  char *desc, size_t desc_size,
                                  float *lat, float *lon)
{
    if (name && name_size) {
        strncpy(name, s_node_name, name_size - 1);
        name[name_size - 1] = '\0';
    }
    if (desc && desc_size) {
        strncpy(desc, s_node_desc, desc_size - 1);
        desc[desc_size - 1] = '\0';
    }
    if (lat) {
        *lat = s_node_lat;
    }
    if (lon) {
        *lon = s_node_lon;
    }
}

// Turns a user-typed node name into a valid, unique-ish mDNS hostname:
// lowercase, only [a-z0-9-], collapsed, truncated. Falls back to
// "ucm-sensor" if the name is empty or ends up with nothing usable.
/**
 * @brief Sanitizes an arbitrary user-typed node name into a string safe
 *        to use as an mDNS hostname label.
 *
 * @param in        Source string (e.g. the raw node name typed into the
 *                   setup form).
 * @param out        Destination buffer for the sanitized hostname.
 * @param out_size   Size of the destination buffer.
 *
 * Rules applied:
 *   - Only alphanumeric characters are kept, lowercased.
 *   - Any run of one or more non-alphanumeric characters (spaces,
 *     punctuation, etc.) is collapsed into a single '-', except leading
 *     runs (a non-alphanumeric character before any real content is
 *     produced yet is simply dropped, since `oi > 0` guards the dash
 *     insertion).
 *   - Trailing dashes are stripped once the whole input has been
 *     processed.
 *   - If nothing usable survives this process (e.g. the input was empty
 *     or entirely punctuation), falls back to the fixed hostname
 *     "ucm-sensor".
 *
 * NOTE: this produces a hostname derived purely from the user-entered
 * node name, with no uniqueness suffix (e.g. no node ID/MAC segment)
 * appended. If two nodes on the same network end up with the same node
 * name (including two nodes both left with no name, which both fall back
 * to the literal "ucm-sensor"), they'll advertise the same mDNS hostname
 * and collide -- only one will reliably be reachable at
 * http://<hostname>.local at a time. Installers should be encouraged to
 * give each node a distinct name to avoid this.
 */
static void sanitize_hostname(const char *in, char *out, size_t out_size)
{
    size_t oi = 0;
    bool last_was_dash = false;
    for (size_t i = 0; in[i] != '\0' && oi + 1 < out_size; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c)) {
            out[oi++] = (char)tolower((unsigned char)c);
            last_was_dash = false;
        } else if (!last_was_dash && oi > 0) {
            out[oi++] = '-';
            last_was_dash = true;
        }
    }
    while (oi > 0 && out[oi - 1] == '-') {
        oi--;
    }
    out[oi] = '\0';

    if (out[0] == '\0') {
        strncpy(out, "ucm-sensor", out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/**
 * @brief Entry point for this module: loads any saved Wi-Fi
 *        credentials/node identity from NVS, then either joins the saved
 *        Wi-Fi network (if credentials exist) or drops into local setup
 *        mode (if they don't, or if joining ultimately fails).
 *
 * @return ESP_OK if this node successfully joined its saved Wi-Fi network
 *         and obtained an IP address (mDNS is also started in this case
 *         before returning). This function does NOT return at all in
 *         the "no saved credentials" or "connection ultimately failed"
 *         cases -- both instead call start_setup_server_and_ap(), which
 *         blocks forever (see that function's docs); the trailing
 *         `return ESP_FAIL;` at the end of this function is therefore
 *         unreachable dead code, left in only to satisfy the function's
 *         declared signature.
 *
 * Called from network_manager.c's network_start() as the fallback when
 * Ethernet doesn't come up within its timeout window.
 *
 * IMPLICIT DEPENDENCY: the esp_netif_init() / esp_event_loop_create_default()
 * calls near the top of this function are commented out, with the
 * implication that both have already been performed earlier in boot --
 * in this project's current call graph, that's true because
 * network_manager.c's network_start() always calls
 * ethernet_manager.c's ethernet_init() first (which performs both of
 * those calls) before ever falling back to this function. If this
 * function were ever called from a different path that skipped Ethernet
 * setup entirely, esp_netif_create_default_wifi_sta() below would likely
 * fail, since it depends on esp_netif having already been initialized.
 *
 * Steps:
 *  1. Creates the Wi-Fi station-mode event group
 *     (s_wifi_event_group) used to signal connection outcome.
 *  2. esp_netif_create_default_wifi_sta() -- creates the default network
 *     interface for STA (station) mode.
 *  3. Opens the "wifi_cfg" NVS namespace read-only and loads:
 *       - SSID/password (have_creds is set true only if a non-empty SSID
 *         was found -- an empty/missing SSID is treated the same as "no
 *         credentials saved" even if a password happened to be present).
 *       - Node name/description directly into the module-level
 *         s_node_name/s_node_desc caches.
 *       - Latitude/longitude, stored as strings, parsed back into floats
 *         via strtof() into s_node_lat/s_node_lon. (strtof() with a NULL
 *         end-pointer here means malformed stored text would silently
 *         parse to 0.0 rather than being flagged as an error -- not
 *         expected in practice since these are only ever written by
 *         save_post_handler(), but worth knowing if NVS content were ever
 *         corrupted or edited externally.)
 *  4. If no usable credentials were found: logs and calls
 *     start_setup_server_and_ap() (never returns).
 *  5. Otherwise: initializes the Wi-Fi driver, registers event_handler()
 *     for both all WIFI_EVENT IDs and specifically IP_EVENT_STA_GOT_IP,
 *     configures STA mode with the loaded SSID/password (WPA2-PSK
 *     assumed as the minimum auth threshold via
 *     wifi_config.sta.threshold.authmode), and starts the Wi-Fi driver --
 *     which, per event_handler()'s WIFI_EVENT_STA_START case, immediately
 *     triggers a connection attempt.
 *  6. Blocks (xEventGroupWaitBits(), portMAX_DELAY -- i.e. no timeout of
 *     its own; the effective time bound comes entirely from
 *     MAXIMUM_RETRY retries inside event_handler()) until either
 *     WIFI_CONNECTED_BIT or WIFI_FAIL_BIT is set.
 *  7. On WIFI_CONNECTED_BIT: builds a sanitized mDNS hostname from the
 *     node's name (sanitize_hostname()), starts mDNS (mdns_init()), sets
 *     the hostname and instance name (falling back to "UCM Sensor
 *     Dashboard" if no description was saved), and advertises an
 *     "_http._tcp" service on port 80 (matching web_server.c's HTTP
 *     server) -- letting the node be reached at http://<hostname>.local
 *     in addition to its raw IP. mDNS failing to initialize is logged as
 *     a warning only, not treated as fatal to the overall connection
 *     result. Returns ESP_OK either way once this step is done.
 *  8. On WIFI_FAIL_BIT (retries exhausted): logs the failure, calls
 *     app_wifi_prov_reset_credentials() (wiping the bad/stale
 *     credentials -- and, per that function's notes, the node's saved
 *     identity fields too), stops the Wi-Fi driver, and falls into
 *     start_setup_server_and_ap() (never returns) so the node can be
 *     re-provisioned without needing physical/manual intervention beyond
 *     power-cycling it into range of a phone.
 */
esp_err_t app_wifi_prov_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

 //   ESP_ERROR_CHECK(esp_netif_init());
 //  ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    char ssid[64] = {0};
    char pass[64] = {0};
    bool have_creds = false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);
        if (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK &&
            strlen(ssid) > 0) {
            nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
            have_creds = true;
        }

        size_t name_len = sizeof(s_node_name);
        size_t desc_len = sizeof(s_node_desc);
        nvs_get_str(nvs, NVS_KEY_NAME, s_node_name, &name_len);
        nvs_get_str(nvs, NVS_KEY_DESC, s_node_desc, &desc_len);

        char lat_str[32] = {0};
        char lon_str[32] = {0};
        size_t lat_len = sizeof(lat_str);
        size_t lon_len = sizeof(lon_str);
        if (nvs_get_str(nvs, NVS_KEY_LAT, lat_str, &lat_len) == ESP_OK) {
            s_node_lat = strtof(lat_str, NULL);
        }
        if (nvs_get_str(nvs, NVS_KEY_LON, lon_str, &lon_len) == ESP_OK) {
            s_node_lon = strtof(lon_str, NULL);
        }

        nvs_close(nvs);
    }

    if (!have_creds) {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials, entering setup mode");
        start_setup_server_and_ap(); // never returns
    }

    ESP_LOGI(TAG, "Using saved Wi-Fi credentials, SSID '%s'", ssid);
    ESP_LOGI(TAG, "Node name '%s', lat=%.5f lon=%.5f", s_node_name, s_node_lat, s_node_lon);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to AP SSID: %s", ssid);

        char hostname[32];
        sanitize_hostname(s_node_name, hostname, sizeof(hostname));

        esp_err_t mdns_err = mdns_init();
        if (mdns_err == ESP_OK) {
            mdns_hostname_set(hostname);
            mdns_instance_name_set(s_node_desc[0] ? s_node_desc : "UCM Sensor Dashboard");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "mDNS started: http://%s.local", hostname);
        } else {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
        }

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
    ESP_LOGW(TAG, "Falling back to setup mode -- clearing saved credentials");
    app_wifi_prov_reset_credentials();
    esp_wifi_stop();
    start_setup_server_and_ap(); // never returns
    return ESP_FAIL; // unreachable
}
