#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_wifi_prov_start(void);

esp_err_t app_wifi_prov_reset_credentials(void);

esp_err_t app_wifi_prov_register_http(httpd_handle_t server);

void app_wifi_prov_get_node_info(
    char *name,
    size_t name_size,
    char *desc,
    size_t desc_size,
    float *lat,
    float *lon
);

#ifdef __cplusplus
}
#endif

