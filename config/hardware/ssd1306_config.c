/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "ssd1306.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) instance DUY NHẤT của SSD1306_t - khai báo extern trong
   ssd1306.h. Không có initializer tường minh - global/static storage duration tự động
   zero-init theo chuẩn C, đủ dùng vì mọi field đều được Oled_Init() (task/oled.c) gán lại
   qua ssd1306_add_i2c_device()/ssd1306_init() trước khi dùng tới. */
SSD1306_t gSsd1306DeviceInfo;
