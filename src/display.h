/** 
 * Author: Jakub Lůčný (xlucnyj00)
 * Date: 10.12.2025
 * 
 * VUT FIT IMP 2025
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "u8g2.h"

#define I2C_TIMEOUT_MS    1000

// Initialize OLED display over I2C and wake it up
void display_init(u8g2_t *u8g2, uint8_t i2c_addr);
// Draw status text
void display_draw_status(u8g2_t *u8g2, const char *line1, const char *line2);
// Animate a simple progress bar
void display_progress_bar(u8g2_t *u8g2);

#endif // DISPLAY_H
