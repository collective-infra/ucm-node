#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ota_manager_init(void);
esp_err_t ota_manager_confirm_running_image(void);
esp_err_t ota_manager_update(const char *firmware_url);

/* Update-in-progress guard shared by every update path (MQTT, web upload). */
bool ota_manager_try_begin(void);
void ota_manager_end(void);
bool ota_manager_is_busy(void);

#endif // OTA_MANAGER_H