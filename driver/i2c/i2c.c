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
 * Khởi tạo I2C_PORT_NUM đúng 1 lần cho toàn hệ thống - xem giải thích đầy đủ trong i2c.h.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t I2c_Init(void)
{
    i2c_master_bus_config_t lBusCfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_PORT_NUM,
        .scl_io_num = CONFIG_SCL_GPIO,
        .sda_io_num = CONFIG_SDA_GPIO,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&lBusCfg, &gI2cBusHandle);
}
