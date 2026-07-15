#ifndef SPI_H
#define SPI_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/spi_master.h"
#include "esp_err.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* SPI host dùng chung cho toàn hệ thống - mọi driver add device lên bus này (vd vs1053.c)
   PHẢI dùng đúng macro này thay vì tự hardcode SPI2_HOST, để đổi host chỉ cần sửa 1 chỗ */
#define SPI_HOST_ID          SPI2_HOST

/* --- Chân SPI vật lý dùng chung cho SPI_HOST_ID - đổi tại đây nếu đấu dây khác board --- */
#define SPI_MOSI_PIN         23
#define SPI_MISO_PIN         19
#define SPI_SCLK_PIN         18

/* Kích thước giao dịch lớn nhất bus cần hỗ trợ (byte) - hiện chỉ VS1053 dùng bus này
   (VS1053_CHUNK_SIZE = 32, xem vs1053.h). Nếu sau này thêm device khác cần gửi/nhận khối lớn
   hơn, tăng giá trị này lên (không giảm - device hiện tại vẫn cần đủ 32) */
#define SPI_MAX_TRANSFER_SIZE   32U

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Spi_Init
 * Khởi tạo SPI_HOST_ID đúng 1 lần cho toàn hệ thống (spi_bus_initialize() sẽ lỗi nếu gọi lại
 * lần 2 trên cùng 1 host) - gọi trong app_main() TRƯỚC KHI tạo bất kỳ task nào cần add device
 * lên bus này (vd Mp3_Task, xem vs1053_init() trong vs1053.c). Chỉ khởi tạo BUS, KHÔNG add
 * device nào cả - mỗi task tự add device riêng của mình (kiến trúc Owner Task, xem srm.h),
 * hàm này chỉ lo phần hạ tầng dùng chung không thuộc về task nào.
 * @param
 * @return ESP_OK nếu khởi tạo bus thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Spi_Init(void);

#endif /* SPI_H */
