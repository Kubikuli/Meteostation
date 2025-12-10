/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include "esp_event.h"

extern EventGroupHandle_t wifi_event_group;

// Initialize NVS storage required by Wi‑Fi for saving credentials
void nvs_init(void);
// Initialize Wi‑Fi in STA mode
void wifi_sta_init(void);
// Load stored SSID/password from NVS; returns true if available
bool wifi_load_creds(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

#endif // WIFI_H
