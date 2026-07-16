/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "spi.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Spi_Init
 * Khởi tạo SPI2_HOST_ID đúng 1 lần cho toàn hệ thống - dùng gSpiBusConfig (định nghĩa trong
 * spi_config.c) thay vì tự khai báo/liệt kê config ở đây, xem thêm giải thích trong spi.h.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Spi_Init(void)
{
    return spi_bus_initialize(SPI2_HOST_ID, &gSpiBusConfig, SPI_DMA_CH_AUTO);
}
