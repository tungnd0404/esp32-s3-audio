#ifndef SPI_H
#define SPI_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/spi_master.h"
#include "esp_err.h"
#include "spi_config.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* SPI2_HOST_ID/SPI2_MOSI_PIN/SPI2_MISO_PIN/SPI2_SCLK_PIN/SPI_MAX_TRANSFER_VS1053_CHUNK_SIZE khai báo tập
   trung trong config/hardware/spi_config.h, không khai báo riêng ở đây nữa - xem file đó để
   đổi board/chân đấu dây. */

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Spi_Init
 * Khởi tạo SPI2_HOST_ID đúng 1 lần cho toàn hệ thống (spi_bus_initialize() sẽ lỗi nếu gọi lại
 * lần 2 trên cùng 1 host) - gọi trong app_main() TRƯỚC KHI tạo bất kỳ task nào cần add device
 * lên bus này (vd Mp3_Task, xem vs1053_init() trong vs1053.c). Chỉ khởi tạo BUS, KHÔNG add
 * device nào cả - mỗi task tự add device riêng của mình (kiến trúc Owner Task, xem srm.h),
 * hàm này chỉ lo phần hạ tầng dùng chung không thuộc về task nào.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Spi_Init(void);

#endif /* SPI_H */
