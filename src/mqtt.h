/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef MQTT_H
#define MQTT_H

#include "mqtt_client.h"

// Global MQTT client handle used for publishing
extern esp_mqtt_client_handle_t mqtt_client;

// Initialize and start MQTT client for given broker URI
void mqtt_start(const char *broker_uri);
// Publish temperature and humidity JSON to specified topic
void mqtt_publish_values(float temp, float hum, const char *topic);

#endif // MQTT_H
