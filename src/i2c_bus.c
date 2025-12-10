/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * Inspiration taken from: https://github.com/espressif/esp-idf/tree/master/examples
 *      file: i2c_basic_example_main.c
 * 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "i2c_bus.h"
#include "esp_err.h"
#include "sdkconfig.h"

// I2C master bus handle shared across modules
i2c_master_bus_handle_t g_i2c_bus = NULL;

// Set up the I2C master bus using default clock and used pins
void i2c_bus_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &g_i2c_bus));
}
