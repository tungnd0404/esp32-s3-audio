#ifndef GPIO_CONFIG_H
#define GPIO_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Cấu hình gpio_config_t dùng chung cho toàn bộ nút bấm (pin_bit_mask gộp cả 3 chân
   BUTTON_xxx_PIN, xem button_config.h) - định nghĩa (cấp phát thật) trong gpio_config.c,
   Button_Init() (driver/button/button.c) chỉ lấy địa chỉ dùng lại thay vì tự khai báo biến
   local. Không có #define nào ở đây - gpio_config.h chỉ khai báo instance gpio_config_t
   (kiểu dữ liệu chung của driver/gpio.h), còn chân vật lý cụ thể vẫn thuộc về module sở hữu
   chân đó (button_config.h). */
extern gpio_config_t gButtonGpioConfig;

#endif /* GPIO_CONFIG_H */
