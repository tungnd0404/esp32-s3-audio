#ifndef I2C_CONFIG_H
#define I2C_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/i2c_master.h"
#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Port I2C dùng chung cho toàn hệ thống - đổi thẳng giá trị này (I2C_NUM_0/I2C_NUM_1) nếu
   board dùng port khác. Bản trước chọn qua 2 macro CONFIG_I2C_PORT_0/CONFIG_I2C_PORT_1 (ON/
   OFF) + #if - bỏ đi vì chỉ có đúng 1 nhánh có thể ON tại 1 thời điểm, define thẳng giá trị
   cuối cùng đơn giản và tường minh hơn */
#define I2C0_PORT_NUM    I2C_NUM_0

/* --- Chân I2C vật lý dùng chung cho I2C0_PORT_NUM - đổi tại đây nếu đấu dây khác board.
   Đặt tên I2C0_ (không phải OLED_) vì đây là chân của BUS dùng chung do I2c_Init() (i2c.c)
   khởi tạo, không thuộc riêng SSD1306 - hiện tại SSD1306 là thiết bị I2C duy nhất trên bus,
   nhưng nếu sau này thêm thiết bị I2C khác, các thiết bị đó vẫn dùng chung đúng 2 chân này.
   Tiền tố "0" ứng với I2C_NUM_0 - nếu sau này cần thêm bus I2C thứ 2 (I2C_NUM_1), thêm bộ
   macro I2C1_PORT_NUM/SDA_PIN/SCL_PIN riêng, không đổi lại bộ này --- */
#define I2C0_SDA_PIN     GPIO_NUM_8
#define I2C0_SCL_PIN     GPIO_NUM_9

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Cấu hình bus I2C dùng chung cho toàn hệ thống - định nghĩa (cấp phát thật) trong
   i2c_config.c, I2c_Init() (driver/i2c/i2c.c) chỉ lấy địa chỉ dùng lại thay vì tự khai báo
   biến local. */
extern i2c_master_bus_config_t gI2cBusConfig;

#endif /* I2C_CONFIG_H */
