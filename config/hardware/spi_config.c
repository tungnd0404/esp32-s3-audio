/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "spi_config.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) cấu hình bus SPI dùng chung - khai báo extern trong
   spi_config.h. quadwp_io_num/quadhd_io_num = -1 (không dùng) vì bus chỉ chạy chế độ SPI
   chuẩn (không Quad SPI); mosi/miso/sclk/max_transfer_sz lấy từ SPI2_MOSI_PIN/MISO_PIN/
   SCLK_PIN/MAX_TRANSFER_SIZE ở trên. */
spi_bus_config_t gSpiBusConfig =
{
    .mosi_io_num = SPI2_MOSI_PIN,
    .miso_io_num = SPI2_MISO_PIN,
    .sclk_io_num = SPI2_SCLK_PIN,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = SPI_MAX_TRANSFER_VS1053_CHUNK_SIZE
};
