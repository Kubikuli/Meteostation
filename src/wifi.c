/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * Inspiration taken from: https://github.com/espressif/esp-idf/tree/master/examples
 *      files: station_example_main.c, nvs_value_example_main.c
 * 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "wifi.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Event group signaling Wi‑Fi connection state
EventGroupHandle_t wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;

// Handle Wi‑Fi and IP events; sets/clears connection bit accordingly
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize NVS storage
void nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }
}

// Initialize Wi‑Fi in STA mode and register event handlers
void wifi_sta_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
}

// Load SSID/password from NVS into provided buffers; returns true on success
bool wifi_load_creds(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len) {
    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READONLY, &nvh) != ESP_OK) {
        return false;
    }

    size_t len_ssid = ssid_len;
    size_t len_pass = pass_len;
    esp_err_t er1 = nvs_get_str(nvh, "ssid", out_ssid, &len_ssid);
    esp_err_t er2 = nvs_get_str(nvh, "pass", out_pass, &len_pass);

    nvs_close(nvh);
    return (er1 == ESP_OK && er2 == ESP_OK && out_ssid[0] != '\0');
}
