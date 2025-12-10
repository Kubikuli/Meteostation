/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>
#include "driver/i2c_master.h"

#define I2C_PORT    I2C_NUM_0
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// Global handle to the initialized I2C master bus
extern i2c_master_bus_handle_t g_i2c_bus;

// Initialize the I2C master bus with configured pins and pull up resistors
void i2c_bus_init(void);

#endif // I2C_BUS_H
