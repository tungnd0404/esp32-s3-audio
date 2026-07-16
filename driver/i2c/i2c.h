#ifndef I2C_H
#define I2C_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_config.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* I2C0_PORT_NUM/I2C0_SDA_PIN/I2C0_SCL_PIN khai báo tập trung trong config/hardware/i2c_config.h,
   không khai báo riêng ở đây nữa - I2c_Init() (i2c.c) và mọi driver add device (vd
   Oled_Init() trong oled.c) đều dùng chung macro đó, đảm bảo cùng 1 port. */

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* I2C bus handle dùng chung cho toàn hệ thống, tạo bởi I2c_Init() - mọi driver add device lên
   bus này (vd Oled_Init() trong oled.c, qua ssd1306_add_i2c_device()) đọc thẳng biến này thay vì tự
   tạo bus riêng (i2c_new_master_bus() chỉ được gọi đúng 1 lần cho cùng 1 port) */
extern i2c_master_bus_handle_t gI2cBusHandle;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief I2c_Init
 * Khởi tạo I2C0_PORT_NUM đúng 1 lần cho toàn hệ thống (i2c_new_master_bus() sẽ lỗi nếu gọi lại
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
