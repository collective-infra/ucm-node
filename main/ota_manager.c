/**
 * @file ota_manager.c
 * @brief OTA boot-state, firmware validation, and firmware update management.
 *
 * This module manages the UCM OTA lifecycle:
 *
 *   1. Identify the currently running OTA partition.
 *   2. Report the running firmware image and OTA state.
 *   3. Detect PENDING_VERIFY firmware.
 *   4. Confirm a successfully validated firmware image.
 *   5. Download and install a new firmware image.
 *
 * OTA download currently uses the UCM-HUB HTTP server:
 *
 *   http://ucm-hub.local/ota/node_test.bin
 *
 * HTTPS can be added later. The current goal is to establish and verify
 * the complete OTA mechanism on the trusted local UCM network.
 *
 * IMPORTANT:
 *
 * A newly installed firmware image enters ESP_OTA_IMG_PENDING_VERIFY.
 * ota_manager_init() deliberately does NOT confirm it.
 *
 * The application must complete its startup/self-tests and then explicitly
 * call ota_manager_confirm_running_image().
 *
 * This preserves the ESP-IDF rollback mechanism.
 */

#include "ota_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_client.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include <stdatomic.h>

#ifndef FW_COMMIT_HASH
#define FW_COMMIT_HASH "unknown"
#endif

static const char *TAG = "UCM_OTA";

static atomic_bool s_busy = false;

bool ota_manager_try_begin(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&s_busy, &expected, true);
}

void ota_manager_end(void)     { atomic_store(&s_busy, false); }
bool ota_manager_is_busy(void) { return atomic_load(&s_busy); }


/**
 * @brief Initialize the OTA manager and report the current boot state.
 *
 * This function does NOT confirm a pending firmware image.
 * Confirmation is performed separately after application health checks
 * have completed.
 */
esp_err_t ota_manager_init(void)
{
    const esp_partition_t *running_partition =
        esp_ota_get_running_partition();

    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Unable to determine running OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Running partition: %s "
             "(type=0x%02x subtype=0x%02x offset=0x%08lx size=0x%08lx)",
             running_partition->label,
             running_partition->type,
             running_partition->subtype,
             (unsigned long)running_partition->address,
             (unsigned long)running_partition->size);

    esp_ota_img_states_t ota_state;

    esp_err_t err = esp_ota_get_state_partition(
        running_partition,
        &ota_state
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to get OTA state: %s",
                 esp_err_to_name(err));
        return err;
    }

    switch (ota_state) {

        case ESP_OTA_IMG_NEW:
            ESP_LOGI(TAG, "OTA image state: NEW");
            break;

        case ESP_OTA_IMG_PENDING_VERIFY:
            ESP_LOGW(TAG,
                     "OTA image is PENDING_VERIFY");
            ESP_LOGW(TAG,
                     "Firmware must pass UCM startup validation "
                     "before being marked valid");
            break;

        case ESP_OTA_IMG_VALID:
            ESP_LOGI(TAG, "OTA image state: VALID");
            break;

        case ESP_OTA_IMG_INVALID:
            ESP_LOGW(TAG, "OTA image state: INVALID");
            break;

        case ESP_OTA_IMG_ABORTED:
            ESP_LOGW(TAG, "OTA image state: ABORTED");
            break;

        case ESP_OTA_IMG_UNDEFINED:
            ESP_LOGI(TAG, "OTA image state: UNDEFINED");
            break;

        default:
            ESP_LOGW(TAG,
                     "OTA image state: unknown (%d)",
                     ota_state);
            break;
    }

    /*
     * We intentionally do NOT call:
     *
     *     esp_ota_mark_app_valid_cancel_rollback();
     *
     * here.
     *
     * A firmware image in PENDING_VERIFY state has not yet passed the
     * UCM application-level health checks.
     */

    return ESP_OK;
}

/**
 * @brief Confirm that the currently running firmware has passed
 * application-level validation.
 *
 * Calling this function marks the current OTA image as valid and
 * cancels the pending rollback mechanism.
 *
 * This function should only be called after the UCM application has
 * successfully completed its required startup checks.
 */
esp_err_t ota_manager_confirm_running_image(void)
{
    esp_err_t err =
        esp_ota_mark_app_valid_cancel_rollback();

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to confirm running firmware: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Running firmware confirmed VALID");

    return ESP_OK;
}

/**
 * @brief Download and install a new firmware image.
 *
 * ESP-IDF selects the inactive OTA partition and writes the downloaded
 * firmware there.
 *
 * After successful download and image validation, ESP-IDF selects the
 * new partition as the next boot partition.
 *
 * The device is then restarted.
 *
 * @param firmware_url HTTP or HTTPS URL of the firmware binary.
 *
 * @return ESP_OK if the OTA image was successfully installed and the
 *         next boot partition was selected. Normally the device restarts
 *         immediately after a successful update.
 */
esp_err_t do_http_update(const char *firmware_url)
{
    if (firmware_url == NULL || firmware_url[0] == '\0') {
        ESP_LOGE(TAG, "OTA firmware URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA update");
    ESP_LOGI(TAG, "Firmware URL: %s", firmware_url);

    /*
     * Find the inactive OTA partition.
     */
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "OTA target partition: %s "
             "(offset=0x%08lx size=0x%08lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    /*
     * Configure HTTP client.
     *
     * This is intentionally HTTP for the current local UCM-HUB test.
     * HTTPS can be added later with proper server verification.
     */
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .user_agent = "UCM-Node (" FW_COMMIT_HASH ")",
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&http_config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    /*
     * Open HTTP connection.
     */
    esp_err_t err = esp_http_client_open(client, 0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to open OTA connection: %s",
                 esp_err_to_name(err));

        esp_http_client_cleanup(client);
        return err;
    }

    /*
     * Get HTTP response headers.
     */
    int content_length =
        esp_http_client_fetch_headers(client);

    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch OTA HTTP headers");

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "OTA image size: %d bytes",
             content_length);

    /*
     * Protect against an image larger than the OTA partition.
     */
    if (content_length > 0 &&
        (size_t)content_length > update_partition->size) {

        ESP_LOGE(TAG,
                 "OTA image is too large for partition");

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return ESP_ERR_INVALID_SIZE;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /*
     * Begin writing the new firmware image.
     */
    esp_ota_handle_t ota_handle = 0;

    err = esp_ota_begin(
        update_partition,
        OTA_SIZE_UNKNOWN,
        &ota_handle
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_ota_begin failed: %s",
                 esp_err_to_name(err));

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return err;
    }

    uint8_t buffer[4096];
    int total_read = 0;

    /*
     * Download the firmware and write it to the inactive
     * OTA partition.
     */
    while (1) {

        int read_len =
            esp_http_client_read(
                client,
                (char *)buffer,
                sizeof(buffer)
            );

        if (read_len < 0) {

            ESP_LOGE(TAG, "HTTP read failed");

            esp_ota_abort(ota_handle);

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            return ESP_FAIL;
        }

        if (read_len == 0) {
            break;
        }

        err = esp_ota_write(
            ota_handle,
            buffer,
            read_len
        );

        if (err != ESP_OK) {

            ESP_LOGE(TAG,
                     "esp_ota_write failed: %s",
                     esp_err_to_name(err));

            esp_ota_abort(ota_handle);

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            return err;
        }

        total_read += read_len;
    }

    ESP_LOGI(TAG,
             "OTA download complete: %d bytes",
             total_read);

    /*
     * If the server supplied a Content-Length, make sure we
     * actually received the complete image.
     */
    if (content_length > 0 &&
        total_read != content_length) {

        ESP_LOGE(TAG,
                 "OTA download incomplete: received %d of %d bytes",
                 total_read,
                 content_length);

        esp_ota_abort(ota_handle);

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return ESP_FAIL;
    }

    /*
     * Finish and validate the OTA image.
     */
    err = esp_ota_end(ota_handle);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "esp_ota_end failed: %s",
                 esp_err_to_name(err));

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return err;
    }

    ESP_LOGI(TAG, "OTA image verified successfully");

    /*
     * Select the newly written partition for the next boot.
     */
    err = esp_ota_set_boot_partition(update_partition);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "Failed to set OTA boot partition: %s",
                 esp_err_to_name(err));

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        return err;
    }

    ESP_LOGI(TAG,
             "Next boot partition set to: %s",
             update_partition->label);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "OTA update successful");
    ESP_LOGI(TAG, "Restarting into new firmware...");

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_restart();

    /*
     * esp_restart() should not return.
     * Keep this here to satisfy the compiler.
     */
    return ESP_OK;
}

esp_err_t ota_manager_update(const char *firmware_url)
{
    if (!ota_manager_try_begin()) {
        ESP_LOGW(TAG, "OTA update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = do_http_update(firmware_url);

    ota_manager_end();   // only reached on failure; success restarts the device
    return err;
}