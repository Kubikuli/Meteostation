/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef PORTAL_H
#define PORTAL_H

#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_netif.h"
#include "u8g2.h"

// Handle to the running HTTP server instance in portal mode
extern httpd_handle_t portal_httpd;
// Netif instance for the SoftAP used by the portal
extern esp_netif_t *portal_ap_netif;

// Flag set when user chooses to run without MQTT
extern volatile bool portal_skip_no_mqtt;

// Start SoftAP + minimal HTTP portal for configuration
void portal_start(void);
// Run portal until user selects skip; cleans up AP and server
void portal_run_blocking(u8g2_t *u8g2);

#endif // PORTAL_H
