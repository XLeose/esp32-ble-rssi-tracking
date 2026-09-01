#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#define TAG_ID          0x01          // Unique Tag ID (Device #1)
#define DEVICE_NAME     "ARGE_TAG_01" // BLE Advertising device name
#define SLEEP_TIME_SEC   30           // Deep sleep duration in seconds
#define STREAM_TIME_MS  150           // Active broadcast duration in milliseconds

static const char *TAG = "TAG_TX";

// Battery level percentage (0-100%)
// NOTE: Currently simulated with a fixed value for prototyping purposes because the test setup
// was powered directly via USB without a battery circuit.
// In a battery-powered deployment, wire a voltage divider to an ADC pin and read the real voltage:
// e.g., adc_oneshot_read(...) -> calculate battery % -> assign to battery_level.
uint8_t battery_level = 85; 

void ble_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));

    // Initialize Device Name and Discovery Flags
    fields.flags = BLE_HS_ADV_F_DISC_GEN;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    // Custom Manufacturer Data -> Carries Tag ID and Battery Level
    // Format: [CompanyID_Low, CompanyID_High, Tag_ID, Battery_Level]
    uint8_t mfg_data[4] = { 0xFF, 0xFF, TAG_ID, battery_level };
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Adv fields could not be set: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON; // Non-connectable beacon mode (broadcast only)
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // Interval units are 0.625 ms: 32 * 0.625 ms = 20 ms.
    // Device broadcasts packets rapidly every 20 ms during its active window.
    adv_params.itvl_min = 32; 
    adv_params.itvl_max = 32;

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    
    ESP_LOGI(TAG, "Tag Broadcast Started -> ID: %d, Battery: %%%d", TAG_ID, battery_level);
}

void ble_app_on_sync(void) {
    ble_advertise();
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "Tag woke up! Broadcasting for %d ms...", STREAM_TIME_MS);
    vTaskDelay(pdMS_TO_TICKS(STREAM_TIME_MS));

    uint64_t sleep = SLEEP_TIME_SEC * 1000000ULL; 
    esp_sleep_enable_timer_wakeup(sleep);
    ESP_LOGI(TAG, "Entering Deep Sleep for %d seconds...", SLEEP_TIME_SEC);
    esp_deep_sleep_start();
}
