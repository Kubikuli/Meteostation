/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * Inspiration taken from: https://github.com/espressif/esp-idf/tree/master/examples
 *      file: mqtt/main/app_main.c
 * 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "mqtt.h"

// Handle to the MQTT client instance
esp_mqtt_client_handle_t mqtt_client = NULL;

// Configure and start MQTT client using given broker URI
void mqtt_start(const char *broker_uri) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client) {
        esp_mqtt_client_start(mqtt_client);
    }
}

// Publish temperature and humidity as JSON
void mqtt_publish_values(float temp, float hum, const char *topic) {
    if (!mqtt_client) return;
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"temp_c\":%.2f,\"hum\":%.2f}", temp, hum);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 0, 0);
}
