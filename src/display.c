/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * Inspiration taken from: https://github.com/espressif/esp-idf/tree/master/examples
 *      file: i2c_u8g2_main.c
 * 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "display.h"
#include "i2c_bus.h"

// I2C device handle for the OLED display
static i2c_master_dev_handle_t display_dev_handle  = NULL;

// Handles all I2C communication between U8G2 library and display controller
// Returns 1 on success, 0 on failure
static uint8_t u8x8_byte_i2c_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    static uint8_t buffer[132];  /*!< Enhanced buffer: control byte + 128 data bytes + margin */
    static uint8_t buf_idx;      /*!< Current buffer index */

    switch(msg) {
        case U8X8_MSG_BYTE_INIT:
            buf_idx = 0;
            break;
        case U8X8_MSG_BYTE_START_TRANSFER:
            buf_idx = 0;
            break;
        case U8X8_MSG_BYTE_SET_DC:
            /* DC (Data/Command) control - handled by SSD1306 protocol */
            break;
        case U8X8_MSG_BYTE_SEND:
            /* Add data bytes to buffer */
            for (size_t i = 0; i < arg_int; ++i) {
                buffer[buf_idx++] = *((uint8_t*)arg_ptr + i);
            }
            break;
        case U8X8_MSG_BYTE_END_TRANSFER:
            if (buf_idx > 0 && display_dev_handle != NULL) {
                esp_err_t ret = i2c_master_transmit(display_dev_handle, buffer, buf_idx, I2C_TIMEOUT_MS);
                if (ret != ESP_OK) {
                    ESP_LOGE("Display", "I2C master transmission failed");
                    return 0;
                }
            }
            break;
        default:
            return 0;
    }
    return 1;
}

// U8X8 GPIO control and delay callback function for ESP32
static uint8_t u8g2_esp32_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        ESP_LOGI("Display", "GPIO and delay initialization completed");
        break;

    case U8X8_MSG_DELAY_MILLI:
        /* Millisecond delay */
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;

    case U8X8_MSG_DELAY_10MICRO:
        /* 10 microsecond delay */
        esp_rom_delay_us(arg_int * 10);
        break;

    case U8X8_MSG_DELAY_100NANO:
        /* 100 nanosecond delay - use minimal delay on ESP32 */
        __asm__ __volatile__("nop");
        break;

    case U8X8_MSG_DELAY_I2C:
        /* I2C timing delay: 5us for 100KHz, 1.25us for 400KHz */
        esp_rom_delay_us(5 / arg_int);
        break;

    case U8X8_MSG_GPIO_RESET:
        /* GPIO reset control (optional for most display controllers) */
        break;

    default:
        /* Other GPIO messages not handled */
        return 0;
    }
    return 1;
}

// Initialize OLED display over I2C and wake it up
void display_init(u8g2_t *u8g2, uint8_t i2c_addr) {
    i2c_device_config_t oled_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = CONFIG_I2C_MASTER_FREQUENCY,
        .flags.disable_ack_check = false,
    };
    i2c_master_bus_add_device(g_i2c_bus, &oled_config, &display_dev_handle);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        u8g2, U8G2_R0,
        u8x8_byte_i2c_cb,
        u8g2_esp32_gpio_delay_cb
    );

    u8x8_SetI2CAddress(&u8g2->u8x8, i2c_addr << 1);
    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0);
}

// Draw status text
void display_draw_status(u8g2_t *u8g2, const char *line1, const char *line2) {
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    if (line1) u8g2_DrawStr(u8g2, 2, 14, line1);
    if (line2) u8g2_DrawStr(u8g2, 2, 28, line2);
    u8g2_SendBuffer(u8g2);
}

// Animate a simple progress bar
void display_progress_bar(u8g2_t *u8g2) {
    u8g2_DrawFrame(u8g2, 10, 60, 108, 4);
    for (int progress = 0; progress <= 100; progress += 5) {
        int fill_width = (progress * 106) / 100;
        u8g2_DrawBox(u8g2, 11, 61, fill_width, 2);
        u8g2_SendBuffer(u8g2);
        vTaskDelay(pdMS_TO_TICKS(75));
    }
}
