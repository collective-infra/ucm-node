/** 
 * Contains the code for the web interface to update the ESP32.
 * 
 * You can go to http://<node-ip>/update on a browser or use curl:
 * 
 *  curl --basic -u admin:<password> -H 'Expect:' \
 *  --data-binary @build-docker/ucm_node.bin http://<node-ip>/update
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "ota_manager.h"
#include "secrets.h"
#include "web_update.h"

static const char *TAG = "WEB_UPDATE";

static const char update_page[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>UCM Firmware Update</title>"
    "<style>body{font-family:Arial,sans-serif;margin:20px}progress{width:100%}</style>"
    "</head><body>"
    "<h1>Firmware update</h1>"
    "<p>Choose the application .bin (not the bootloader or a merged image). "
    "The node reboots when the upload finishes.</p>"
    "<input type='file' id='f' accept='.bin'> <button id='b'>Upload</button>"
    "<p><progress id='p' value='0' max='100'></progress></p><p id='s'></p>"
    "<script>"
    "document.getElementById('b').onclick=function(){"
    "var f=document.getElementById('f').files[0];if(!f){return;}"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable){"
    "document.getElementById('p').value=100*e.loaded/e.total;}};"
    "x.onload=function(){document.getElementById('s').textContent=x.status+': '+x.responseText;};"
    "x.onerror=function(){document.getElementById('s').textContent='Upload failed';};"
    "x.send(f);};"
    "</script></body></html>";

static bool ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool check_auth(httpd_req_t *req)
{
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }

    char creds[96];
    snprintf(creds, sizeof(creds), "%s:%s", WEB_UPDATE_USER, WEB_UPDATE_PASSWORD);

    unsigned char b64[160];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen,
                              (const unsigned char *)creds, strlen(creds)) != 0) {
        return false;
    }

    char expected[192];
    snprintf(expected, sizeof(expected), "Basic %.*s", (int)olen, b64);
    return ct_equal(hdr, expected);
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"UCM node\"");
    return httpd_resp_sendstr(req, "Unauthorized\n");
}

static esp_err_t update_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return send_unauthorized(req);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, update_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return send_unauthorized(req);
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Bad request or image too large");
    }

    if (!ota_manager_try_begin()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Another update is in progress\n");
    }

    ESP_LOGW(TAG, "Web update started: %d bytes -> %s", req->content_len, part->label);

    esp_ota_handle_t handle;
    char *buf = malloc(4096);
    if (!buf || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        free(buf);
        ota_manager_end();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || esp_ota_write(handle, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "Upload/write failed");
            esp_ota_abort(handle);
            free(buf);
            ota_manager_end();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        }
        remaining -= n;
    }
    free(buf);

    // esp_ota_end() validates the image; boot partition only changes if it passes
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid firmware image");
        ota_manager_end();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
    }

    ESP_LOGW(TAG, "Web update OK, rebooting");
    httpd_resp_sendstr(req, "Update OK, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_update_register(httpd_handle_t server)
{
    static const httpd_uri_t get_uri = {
        .uri = "/update", .method = HTTP_GET, .handler = update_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/update", .method = HTTP_POST, .handler = update_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &get_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &post_uri);
    }
    return err;
}