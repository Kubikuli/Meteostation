/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * Inspiration taken from: https://github.com/espressif/esp-idf/tree/master/examples
 *      files: http_server/simple/main/main.c, http_server/captive_portal/main/main.c
 * 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "portal.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "display.h"

#ifndef CONFIG_AP_SSID
#define CONFIG_AP_SSID "ESP_Config"
#endif

// HTTP server handle, AP netif
httpd_handle_t portal_httpd = NULL;
esp_netif_t *portal_ap_netif = NULL;
volatile bool portal_skip_no_mqtt = false;      /*!< Flag to skip MQTT and run without it */

// Simple HTML form to configure Wi‑Fi or skip MQTT
static esp_err_t root_get_handler(httpd_req_t *req) {
    const char *html =
        "<html><head><meta name=viewport content=\"width=device-width, initial-scale=1\"></head><body>"
        "<h3>Wi-Fi Setup</h3>"
        "<form method='POST' action='/save'>"
        "SSID: <input name='ssid'><br>"
        "Password: <input name='pass' type='password'><br>"
        "<button type='submit'>Save</button>"
        "</form>"
        "<hr>"
        "<form method='GET' action='/skip'>"
        "<button type='submit'>Run without MQTT</button>"
        "</form>"
        "<p>This starts the program without WiFi and MQTT publishing setup.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Simple URL decoder for '+' (space) and special %HH sequences
static void urldecode(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) {
            int hi = src[1];
            int lo = src[2];
            hi = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
            lo = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
            if (hi >= 0 && lo >= 0) { *dst++ = (char)((hi << 4) | lo); src += 3; }
            else { *dst++ = *src++; }
        }
        else { *dst++ = *src++; }
    }
    *dst = '\0';
}

// Handle POST /save: store SSID/pass in NVS and reboot
static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[160];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    buf[received] = '\0';
    char ssid[64] = {0}, pass[64] = {0};
    (void)httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    (void)httpd_query_key_value(buf, "pass", pass, sizeof(pass));
    urldecode(ssid);
    urldecode(pass);

    // Save credentials to NVS
    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_set_str(nvh, "ssid", ssid);
        nvs_set_str(nvh, "pass", pass);
        nvs_commit(nvh);
        nvs_close(nvh);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<p>Saved credentials. Rebooting...</p>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// Handle GET /skip: set flag to run without MQTT
static esp_err_t skip_get_handler(httpd_req_t *req) {
    portal_skip_no_mqtt = true;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<p>Starting without Wifi/MQTT data publishing...</p>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Start SoftAP and HTTP portal with configured IP and SSID
void portal_start(void) {
    portal_ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info = {0};
    ip4_addr_t t;
    IP4_ADDR(&t, 192, 168, 4, 1); ip_info.ip.addr = t.addr;
    IP4_ADDR(&t, 192, 168, 4, 1); ip_info.gw.addr = t.addr;
    IP4_ADDR(&t, 255, 255, 255, 0); ip_info.netmask.addr = t.addr;

    esp_netif_dhcps_stop(portal_ap_netif);
    esp_netif_set_ip_info(portal_ap_netif, &ip_info);
    esp_netif_dhcps_start(portal_ap_netif);

    wifi_config_t ap_cfg = {0};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", CONFIG_AP_SSID, mac[4], mac[5]);
    strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen((char*)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port = 80;
    httpd_start(&portal_httpd, &conf);

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(portal_httpd, &root);
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(portal_httpd, &save);
    httpd_uri_t skip = { .uri = "/skip", .method = HTTP_GET, .handler = skip_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(portal_httpd, &skip);
}

// Run portal until skip flag is set; then clean up AP/server
void portal_run_blocking(u8g2_t *u8g2) {
    display_draw_status(u8g2, "Open portal", "Connect to AP");
    if (portal_httpd) {
        httpd_stop(portal_httpd);
        portal_httpd = NULL;
    }
    portal_start();

    char line1[40];
    uint8_t mac_show[6];
    esp_read_mac(mac_show, ESP_MAC_WIFI_STA);
    char ap_ssid_show[32];
    snprintf(ap_ssid_show, sizeof(ap_ssid_show), "%s-%02X%02X", CONFIG_AP_SSID, mac_show[4], mac_show[5]);
    snprintf(line1, sizeof(line1), "SSID: %s", ap_ssid_show);
    display_draw_status(u8g2, line1, "http://192.168.4.1");


    while (!portal_skip_no_mqtt) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (portal_httpd) {
        httpd_stop(portal_httpd);
        portal_httpd = NULL;
    }
    if (portal_ap_netif) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        esp_netif_destroy(portal_ap_netif);
        portal_ap_netif = NULL;
    }
}
