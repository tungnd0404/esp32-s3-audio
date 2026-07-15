#ifndef I2C_H
#define I2C_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "config.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Port I2C dùng chung cho toàn hệ thống, chọn theo CONFIG_I2C_PORT_0/1 (config.h) - cùng
   khuôn mẫu lựa chọn port trong driver/ssd1306/ssd1306_i2c_new.c, tách ra đây để I2c_Init()
   (i2c.c) và mọi driver add device (vd Oled_Init() trong oled.c) dùng chung đúng 1 port */
#if (CONFIG_I2C_PORT_0 == ON)
#define I2C_PORT_NUM    I2C_NUM_0
#endif

#if (CONFIG_I2C_PORT_1 == ON)
#define I2C_PORT_NUM    I2C_NUM_1
#endif

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* I2C bus handle dùng chung cho toàn hệ thống, tạo bởi I2c_Init() - mọi driver add device lên
   bus này (vd Oled_Init() trong oled.c, qua i2c_device_add()) đọc thẳng biến này thay vì tự
   tạo bus riêng (i2c_new_master_bus() chỉ được gọi đúng 1 lần cho cùng 1 port) */
extern i2c_master_bus_handle_t gI2cBusHandle;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief I2c_Init
 * Khởi tạo I2C_PORT_NUM đúng 1 lần cho toàn hệ thống (i2c_new_master_bus() sẽ lỗi nếu gọi lại
 * lần 2 trên cùng 1 port) - gọi trong app_main() TRƯỚC KHI tạo bất kỳ task nào cần add device
 * lên bus này (vd Oled_Task, xem Oled_Init() trong oled.c). Chỉ khởi tạo BUS, KHÔNG add
 * device nào cả - mỗi task tự add device riêng của mình (kiến trúc Owner Task, xem srm.h),
 * hàm này chỉ lo phần hạ tầng dùng chung không thuộc về task nào. Cùng khuôn mẫu Spi_Init()
 * (driver/spi/spi.c).
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t I2c_Init(void);

#endif /* I2C_H */
