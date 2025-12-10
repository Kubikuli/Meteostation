/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef SENSOR_H
#define SENSOR_H

// Initialize the SHT31 device on the I2C bus
void sensor_init(void);
// Read temperature (°C) and humidity (%) from SHT31
void sensor_read(float *temp_c, float *rh);

#endif // SENSOR_H
