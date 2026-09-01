// Standard ESP-IDF and FreeRTOS Headers
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_mac.h"

// Wi-Fi and MQTT Headers
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

// NimBLE Bluetooth Low Energy Headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Load credentials from credentials.h if present, otherwise fallback to placeholders
#if __has_include("credentials.h")
#include "credentials.h"
#else
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"
#define BROKER_URI      "mqtt://YOUR_MQTT_BROKER_IP"
#define BROKER_USERNAME "YOUR_MQTT_USERNAME"
#define BROKER_PASS     "YOUR_MQTT_PASSWORD"
#define DEVICE_NAME     "YOUR_SCANNER_DEVICE_NAME"
#endif

// Whitelist of BLE Tag names to track
static const char *WHITELIST_TAGS[] = {
    "ARGE_TAG_01",
    "ARGE_TAG_02",
    "ARGE_TAG_03"
};

static const char *TAG = "SCANNER_GATEWAY";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static const int WHITELIST_SIZE = sizeof(WHITELIST_TAGS) / sizeof(WHITELIST_TAGS[0]);
static TickType_t last_send_times[sizeof(WHITELIST_TAGS) / sizeof(WHITELIST_TAGS[0])] = {0};

// Exponential Moving Average (EMA) Filter for RSSI signal smoothing
// Alpha is between 0.0 and 1.0 (lower values reduce fluctuations from multipath fading)
const float ALPHA = 0.15f; 
static float filtered_rssi = 0.0f;

void filter_signal(int raw_rssi) {
    if (filtered_rssi == 0.0f) {
        filtered_rssi = (float)raw_rssi; 
    } else {
        filtered_rssi = (ALPHA * (float)raw_rssi) + ((1.0f - ALPHA) * filtered_rssi);
    }
}

// BLE GAP Discovery Event Callback
static int ble_gap_event(struct ble_gap_event *event, void *arg) 
{
    if (event->type == BLE_GAP_EVENT_DISC) 
    {
        // Parse incoming BLE advertisement packet fields
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

        if (rc == 0 && fields.name != NULL) 
        {
            for (int i = 0; i < WHITELIST_SIZE; i++) 
            {
                // Check if the received name length and string match the whitelist entry
                if (fields.name_len == strlen(WHITELIST_TAGS[i]) && 
                    strncmp((char*)fields.name, WHITELIST_TAGS[i], fields.name_len) == 0) 
                {
                    // Rate limiting logic: Check if 1 second has passed for this specific tag
                    TickType_t now = xTaskGetTickCount();
                    if ((now - last_send_times[i]) >= pdMS_TO_TICKS(1000) || last_send_times[i] == 0) 
                    {
                        last_send_times[i] = now; // Update timestamp for this tag

                        int raw_rssi = event->disc.rssi;
                        filter_signal(raw_rssi);
                        ESP_LOGI(TAG, "Target Device Found: %s | Raw RSSI: %d | Filtered RSSI: %d dBm", 
                                 WHITELIST_TAGS[i], raw_rssi, (int)filtered_rssi);
                    
                        // Publish telemetry JSON to MQTT broker if connected
                        if (mqtt_client != NULL) 
                        {
                            char payload[128];
                            snprintf(payload, sizeof(payload), 
                                     "{\"tag\":\"%s\", \"scanner\":\"%s\", \"rssi\":%d}", 
                                     WHITELIST_TAGS[i], DEVICE_NAME, (int)filtered_rssi);
                            
                            esp_mqtt_client_publish(mqtt_client, "arge/test_device", payload, 0, 1, 0);
                            ESP_LOGI(TAG, "MQTT Packet Published -> %s", payload);
                        }
                    }
                    break; 
                }
            }
        }
    }
    return 0;
}

// Continuous BLE Scanning Configuration
void ble_app_scan(void) 
{
    struct ble_gap_disc_params disc_params;
    // Units are 0.625 ms: 48 * 0.625 ms = 30 ms.
    // Setting window equal to interval ensures continuous scanning without blind spots.
    disc_params.filter_duplicates = 0; 
    disc_params.passive = 1; // Passive listening (avoids active scan request traffic)
    disc_params.itvl = 48;
    disc_params.window = 48;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
    ESP_LOGI(TAG, "BLE Continuous Scanner Started");
}

// NimBLE Host Synchronization Callback
void ble_app_on_sync(void) 
{
    ble_app_scan();
}

// NimBLE Host Task
void ble_host_task(void *param) 
{
    ESP_LOGI(TAG, "NimBLE Task Running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// MQTT Event Handler Callback
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) 
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) 
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Client Successfully Connected to Broker");
            
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg), "{\"state\":\"active\", \"device\":\"%s\"}", DEVICE_NAME);
            int msg_id = esp_mqtt_client_publish(client, "arge/test_device", status_msg, 0, 1, 0);
            ESP_LOGI(TAG, "Initial Status Published, msg_id=%d", msg_id);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Connection Lost from Broker");
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT Message Published Successfully (msg_id=%d)", event->msg_id);
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error Occurred");
            break;
            
        default:
            break;
    }
}

// Wi-Fi and IP Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        ESP_LOGI(TAG, "Wi-Fi Station Started, Connecting...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        ESP_LOGW(TAG, "Wi-Fi Connection Lost, Reconnecting...");
        esp_wifi_connect();
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Address Assigned: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Generate Unique Client ID using the last 3 bytes of the device MAC address
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char client_id[24];
        snprintf(client_id, sizeof(client_id), "ESP_%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Generated MQTT Client ID: %s", client_id);

        // Initialize and start MQTT Client only once upon obtaining IP
        if (mqtt_client == NULL) {
            esp_mqtt_client_config_t mqtt_cfg = {
                .broker.address.uri = BROKER_URI,
                .broker.address.port = 1883,
                .credentials.client_id = client_id,
            };

            // Set authentication if credentials are provided
            if (strlen(BROKER_USERNAME) > 0 && strcmp(BROKER_USERNAME, "YOUR_MQTT_USERNAME") != 0) {
                mqtt_cfg.credentials.username = BROKER_USERNAME;
                mqtt_cfg.credentials.authentication.password = BROKER_PASS;
            }

            mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
            esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(mqtt_client);
        }
    }
}

void app_main(void)
{
    // Initialize NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Network Interface and Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Wi-Fi & IP Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Wi-Fi Station Configuration
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi STA Initialized, attempting to connect to SSID: %s", WIFI_SSID);

    // Initialize NimBLE Bluetooth Stack
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);
}
