#ifndef SSD1306_CONFIG_H
#define SSD1306_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Kích thước màn hình SSD1306 (pixel) */
#define SSD1306_WIDTH      128
#define SSD1306_HEIGHT     64
#define SSD1306_OFFSETX    0

/* Chân reset phần cứng riêng cho SSD1306 (khác chân RESET của VS1053, xem vs1053_config.h) -
   GPIO_NUM_NC (không nối) nếu board không đấu dây reset riêng cho màn hình, dùng reset qua
   I2C/software thay thế (giá trị < 0 khiến ssd1306_add_i2c_device() bỏ qua bước reset, xem
   ssd1306_i2c.c) */
#define SSD1306_RESET_PIN  GPIO_NUM_NC

#endif /* SSD1306_CONFIG_H */
