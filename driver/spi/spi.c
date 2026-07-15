/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "spi.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Spi_Init
 * Khởi tạo SPI_HOST_ID đúng 1 lần cho toàn hệ thống - xem giải thích đầy đủ trong spi.h.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Spi_Init(void)
{
    spi_bus_config_t lBusCfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER_SIZE
    };

    return spi_bus_initialize(SPI_HOST_ID, &lBusCfg, SPI_DMA_CH_AUTO);
}
