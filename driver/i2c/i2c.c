/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "i2c.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

i2c_master_bus_handle_t gI2cBusHandle = NULL;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief I2c_Init
 * Khởi tạo I2C0_PORT_NUM đúng 1 lần cho toàn hệ thống - dùng gI2cBusConfig (định nghĩa trong
 * i2c_config.c) thay vì tự khai báo/liệt kê config ở đây, xem thêm giải thích trong i2c.h.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t I2c_Init(void)
{
    return i2c_new_master_bus(&gI2cBusConfig, &gI2cBusHandle);
}
