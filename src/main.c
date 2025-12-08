#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "u8g2.h"
#include "mqtt_client.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"

#define TAG "Meteostation"

#define I2C_PORT    I2C_NUM_0
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_FREQ    CONFIG_I2C_MASTER_FREQUENCY
#define DISPLAY_ADDR CONFIG_I2C_DISPLAY_ADDRESS

#define SHT31_ADDR 0x44     //< SHT31 I2C address

#ifndef CONFIG_AP_SSID
#define CONFIG_AP_SSID "ESP_Config"
#endif

#define WIFI_CONNECT_TIMEOUT_MS 10000

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtt://broker.hivemq.com:1883"
#endif
#ifndef CONFIG_MQTT_TOPIC
#define CONFIG_MQTT_TOPIC "meteostanice/measurements"
#endif

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t s_mqtt = NULL;
static httpd_handle_t s_http = NULL;
static esp_netif_t *s_ap_netif = NULL;
static volatile bool s_skip_no_mqtt = false; // set when user chooses to run without MQTT via portal

static bool load_wifi_creds(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len) {
    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READONLY, &nvh) != ESP_OK) return false;
    size_t len_ssid = ssid_len;
    size_t len_pass = pass_len;
    esp_err_t er1 = nvs_get_str(nvh, "ssid", out_ssid, &len_ssid);
    esp_err_t er2 = nvs_get_str(nvh, "pass", out_pass, &len_pass);
    nvs_close(nvh);
    if (er1 == ESP_OK && er2 == ESP_OK && out_ssid[0] != '\0') return true;
    return false;
}

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
        "<p>This starts the thermometer without Wi‑Fi/MQTT.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Minimal URL decoder for '+' and %HH used after query extraction
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
        } else { *dst++ = *src++; }
    }
    *dst = '\0';
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[160];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';
    char ssid[64] = {0}, pass[64] = {0};

    /* httpd_query_key_value works on any key=value&key2=value2 string.
       For application/x-www-form-urlencoded POST bodies, we can reuse it
       to extract decoded values without custom parsing. */
    (void)httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    (void)httpd_query_key_value(buf, "pass", pass, sizeof(pass));
    urldecode(ssid);
    urldecode(pass);

    ESP_LOGI(TAG, "Saving Wi-Fi credentials: SSID='%s', PASS='%s'", ssid, pass);

    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_set_str(nvh, "ssid", ssid);
        nvs_set_str(nvh, "pass", pass);
        nvs_commit(nvh);
        nvs_close(nvh);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<p>Saved. Rebooting...</p>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t skip_get_handler(httpd_req_t *req) {
    s_skip_no_mqtt = true;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<p>Starting without MQTT...</p>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_config_portal(void) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    // 192.168.4.1/24
    esp_netif_ip_info_t ip_info = {0};
    ip4_addr_t t;
    IP4_ADDR(&t, 192, 168, 4, 1); ip_info.ip.addr = t.addr;
    IP4_ADDR(&t, 192, 168, 4, 1); ip_info.gw.addr = t.addr;
    IP4_ADDR(&t, 255, 255, 255, 0); ip_info.netmask.addr = t.addr;
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port = 80;
    ESP_ERROR_CHECK(httpd_start(&s_http, &conf));

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_http, &root);
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_http, &save);
    httpd_uri_t skip = { .uri = "/skip", .method = HTTP_GET, .handler = skip_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_http, &skip);
}

static void draw_status(u8g2_t *u8g2, const char *line1, const char *line2) {
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    if (line1) u8g2_DrawStr(u8g2, 2, 14, line1);
    if (line2) u8g2_DrawStr(u8g2, 2, 28, line2);
    u8g2_SendBuffer(u8g2);
}

static void start_portal_and_block(u8g2_t *u8g2) {
    draw_status(u8g2, "Open portal", "Connect to AP");
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    start_config_portal();
    char line1[40];
    snprintf(line1, sizeof(line1), "SSID: %s", CONFIG_AP_SSID);
    draw_status(u8g2, line1, "http://192.168.4.1");
    while (!s_skip_no_mqtt) { vTaskDelay(pdMS_TO_TICKS(200)); }
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    if (s_ap_netif) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
}

static void mqtt_publish(float temp, float hum) {
    if (!s_mqtt) return;
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"temp_c\":%.2f,\"hum\":%.2f}", temp, hum);
    esp_mqtt_client_publish(s_mqtt, CONFIG_MQTT_TOPIC, payload, 0, 1, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static uint8_t i2c_buffer[256];

uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static size_t index = 0;
    switch(msg) {
        case U8X8_MSG_BYTE_INIT: index = 0; break;
        case U8X8_MSG_BYTE_START_TRANSFER: index = 0; break;
        case U8X8_MSG_BYTE_SEND:
            if (arg_int > 0 && (index + arg_int) < sizeof(i2c_buffer)) {
                memcpy(&i2c_buffer[index], arg_ptr, arg_int);
                index += arg_int;
            }
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (index > 0) {
                i2c_cmd_handle_t cmd = i2c_cmd_link_create();
                i2c_master_start(cmd);
                i2c_master_write_byte(cmd, (DISPLAY_ADDR << 1) | I2C_MASTER_WRITE, true);
                i2c_master_write(cmd, i2c_buffer, index, true);
                i2c_master_stop(cmd);
                i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(1000));
                i2c_cmd_link_delete(cmd);
            }
            break;
        default: return 0;
    }
    return 1;
}

// ------- SHT31 helpers -------
static esp_err_t i2c_write_bytes(uint8_t addr, const uint8_t *bytes, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (len)
        i2c_master_write(cmd, (uint8_t*)bytes, len, true);
    i2c_master_stop(cmd);
    esp_err_t res = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return res;
}

static esp_err_t i2c_read_bytes(uint8_t addr, uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t res = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return res;
}

static void sht31_read(float *temp_c, float *rh) {
    // Single shot high repeatability (0x2400), wait 30ms, read 6 bytes
    const uint8_t cmd[2] = { 0x24, 0x00 };
    i2c_write_bytes(SHT31_ADDR, cmd, 2);
    vTaskDelay(pdMS_TO_TICKS(30));
    
    uint8_t raw[6];
    i2c_read_bytes(SHT31_ADDR, raw, 6);
    
    uint16_t st = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t srh = ((uint16_t)raw[3] << 8) | raw[4];
    *temp_c = -45.0f + 175.0f * ((float)st / 65535.0f);
    *rh = 100.0f * ((float)srh / 65535.0f);
}

uint8_t u8g2_esp32_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch(msg) {
        case U8X8_MSG_DELAY_MILLI: vTaskDelay(pdMS_TO_TICKS(arg_int)); break;
        case U8X8_MSG_DELAY_10MICRO: vTaskDelay(1); break;
        default: return 1;
    }
    return 1;
}

void display_progress_bar(u8g2_t *u8g2) {
    /* Progress bar frame */
    u8g2_DrawFrame(u8g2, 10, 60, 108, 4);

    for (int progress = 0; progress <= 100; progress += 5) {
        /* Progress bar fill */
        int fill_width = (progress * 106) / 100;
        u8g2_DrawBox(u8g2, 11, 61, fill_width, 2);

        u8g2_SendBuffer(u8g2);
        vTaskDelay(pdMS_TO_TICKS(75));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting Meteostation with Wi-Fi + MQTT");
    // ESP_LOGW(TAG, "TESTING: Erasing NVS at boot");
    // ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    s_wifi_events = xEventGroupCreate();
    
    // Configure I2C
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ,
    };
    i2c_param_config(I2C_PORT, &i2c_config);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    
    // Initialize OLED
    u8g2_t u8g2;
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_delay_cb);
    u8x8_SetI2CAddress(&u8g2.u8x8, DISPLAY_ADDR << 1);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);

    // Load stored credentials from NVS (provisioning portal)
    char ssid[64] = {0}, pass[64] = {0};
    bool have_nvs = load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!have_nvs) {
        // No credentials found; start config portal immediately
        start_portal_and_block(&u8g2);
    }

    if (!s_skip_no_mqtt) {
        // Try Wi‑Fi STA connect; if fails, open config portal
        wifi_config_t sta = {0};
        strncpy((char*)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strncpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
        draw_status(&u8g2, "Wi-Fi connecting...", NULL);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            // Connection failed; start SoftAP + portal and block
            start_portal_and_block(&u8g2);
        }

        if (!s_skip_no_mqtt) {
            draw_status(&u8g2, "Wi-Fi connected", NULL);

            // MQTT client setup
            esp_mqtt_client_config_t mqtt_cfg = {
                .broker.address.uri = CONFIG_MQTT_BROKER_URI,
            };
            s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
            if (s_mqtt) {
                esp_mqtt_client_start(s_mqtt);
            }
        }
    } else {
        draw_status(&u8g2, "Bypass: No Wi-Fi/MQTT", NULL);
    }

    while (1) {
        // Read SHT31
        float temp, hum;
        sht31_read(&temp, &hum);

        // Display temperature
        u8g2_ClearBuffer(&u8g2);
        u8g2_SetFont(&u8g2, u8g2_font_ncenB14_tr);

        char line[16];
        snprintf(line, sizeof(line), "%.1f  C", temp);
        u8g2_DrawStr(&u8g2, 15, 38, line);

        // Circle serving as degree symbol
        u8g2_DrawCircle(&u8g2, 59, 25, 2, U8G2_DRAW_ALL);

        // Icon of a thermometer
        // Generated from free icon at https://javl.github.io/image2cpp/
        const uint8_t thermo_bitmap[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x01, 
            0x00, 0x00, 0x00, 0x00, 0x18, 0x03, 0x00, 0x00, 0x00, 0x00, 0x58, 0x3a, 0x00, 0x00, 0x00, 0x00, 
            0x58, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x1a, 0x00, 0x00, 
            0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x7a, 
            0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x1e, 0x00, 0x00, 0x00, 0x00, 
            0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x3a, 0x00, 0x00, 
            0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 
            0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x58, 0x7a, 0x00, 0x00, 0x00, 0x00, 
            0x48, 0x02, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x06, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x05, 0x00, 0x00, 
            0x00, 0x00, 0xf6, 0x09, 0x00, 0x00, 0x00, 0x00, 0xfa, 0x0b, 0x00, 0x00, 0x00, 0x00, 0xfa, 0x0b, 
            0x00, 0x00, 0x00, 0x00, 0xf4, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x44, 0x04, 0x00, 0x00, 0x00, 0x00, 
            0x18, 0x03, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        // Display bitmap thermometer
        u8g2_DrawXBM(&u8g2, 80, 8, 48, 48, thermo_bitmap);
        u8g2_SendBuffer(&u8g2);

        // Wait 3 seconds with progress bar
        display_progress_bar(&u8g2);

        // vTaskDelay(pdMS_TO_TICKS(3000));
        u8g2_ClearBuffer(&u8g2);


        // Display humidity
        snprintf(line, sizeof(line), "%.1f %%", hum);
        u8g2_DrawStr(&u8g2, 15, 38, line);

        // u8g2_SendBuffer(&u8g2);

        // Icon of a humidity
        // Generated from free icon at https://javl.github.io/image2cpp/
        const uint8_t humidity_bitmap[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x03, 0x00, 0x00, 
            0x00, 0x00, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x10, 0x08, 
            0x00, 0x00, 0x00, 0x00, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x30, 0x00, 0x00, 0x00, 0x00, 
            0x04, 0x60, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x00, 0x00, 
            0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc0, 0x00, 0x00, 
            0x03, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x60, 0x00, 0x00, 0x06, 0x00, 0x00, 0x20, 
            0x00, 0x00, 0x04, 0x00, 0x00, 0x20, 0x00, 0x00, 0x04, 0x00, 0x00, 0x10, 0x70, 0x04, 0x08, 0x00, 
            0x00, 0x10, 0x48, 0x04, 0x08, 0x00, 0x00, 0x10, 0x48, 0x02, 0x18, 0x00, 0x00, 0x18, 0x48, 0x02, 
            0x10, 0x00, 0x00, 0x08, 0x70, 0x01, 0x10, 0x00, 0x00, 0x08, 0x00, 0x1d, 0x10, 0x00, 0x00, 0x08, 
            0x80, 0x14, 0x10, 0x00, 0x00, 0x08, 0x80, 0x22, 0x10, 0x00, 0x00, 0x08, 0x40, 0x14, 0x10, 0x00, 
            0x00, 0x08, 0x40, 0x1c, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00, 0x10, 0x00, 0x00, 
            0x08, 0x00, 0x00, 0x30, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x20, 0x00, 0x00, 0x04, 0x00, 0x00, 0x40, 
            0x00, 0x00, 0x02, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0xc0, 0x00, 0x00, 
            0x00, 0x00, 0x1e, 0x78, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        // Display bitmap humidity
        u8g2_DrawXBM(&u8g2, 80, 8, 48, 48, humidity_bitmap);
        u8g2_SendBuffer(&u8g2);

        ESP_LOGI(TAG, "T=%.2fC H=%.2f%%", temp, hum);
        mqtt_publish(temp, hum);

        // Wait 3 seconds with progress bar
        display_progress_bar(&u8g2);
        // vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
