/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "i2c_config.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) cấu hình bus I2C dùng chung - khai báo extern trong
   i2c_config.h. clk_source/glitch_ignore_cnt/flags.enable_internal_pullup là tham số cố
   định của bus (không phải chân/port nên không tách thành macro riêng), i2c_port/
   scl_io_num/sda_io_num lấy từ I2C0_PORT_NUM/I2C0_SCL_PIN/I2C0_SDA_PIN ở trên. */
i2c_master_bus_config_t gI2cBusConfig =
{
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .i2c_port = I2C0_PORT_NUM,
    .scl_io_num = I2C0_SCL_PIN,
    .sda_io_num = I2C0_SDA_PIN,
    .flags.enable_internal_pullup = true,
};
