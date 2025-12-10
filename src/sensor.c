/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#include "freertos/FreeRTOS.h"
#include "sensor.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"

#define SHT31_ADDR 0x44

// I2C device handle for the SHT31 sensor
static i2c_master_dev_handle_t s_i2c_sht31 = NULL;

// Add SHT31 device to the I2C bus
void sensor_init(void) {
    i2c_device_config_t sht_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT31_ADDR,
        .scl_speed_hz = CONFIG_I2C_MASTER_FREQUENCY,
    };
    i2c_master_bus_add_device(g_i2c_bus, &sht_cfg, &s_i2c_sht31);
}

// Trigger measurement and convert raw values to °C and % humidity
void sensor_read(float *temp_c, float *rh) {
    const uint8_t cmd[2] = { 0x24, 0x00 };

    if (!s_i2c_sht31) {
        *temp_c = 0; *rh = 0;
        return;
    }

    if (i2c_master_transmit(s_i2c_sht31, cmd, sizeof(cmd), 1000) != ESP_OK) {
        *temp_c = 0; *rh = 0;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    uint8_t raw[6] = {0};

    if (i2c_master_receive(s_i2c_sht31, raw, sizeof(raw), 1000) != ESP_OK) {
        *temp_c = 0; *rh = 0;
        return;
    }

    uint16_t st = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t srh = ((uint16_t)raw[3] << 8) | raw[4];

    *temp_c = -45.0f + 175.0f * ((float)st / 65535.0f);
    *rh = 100.0f * ((float)srh / 65535.0f);
}
