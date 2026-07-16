#ifndef SPI_CONFIG_H
#define SPI_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/spi_master.h"
#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* SPI host dùng chung cho toàn hệ thống - mọi driver add device lên bus này (vd vs1053.c)
   PHẢI dùng đúng macro này thay vì tự hardcode SPI2_HOST, để đổi host chỉ cần sửa 1 chỗ */
#define SPI2_HOST_ID          SPI2_HOST

/* --- Chân SPI vật lý dùng chung cho SPI2_HOST_ID - đổi tại đây nếu đấu dây khác board --- */
#define SPI2_MOSI_PIN         GPIO_NUM_4
#define SPI2_MISO_PIN         GPIO_NUM_19
#define SPI2_SCLK_PIN         GPIO_NUM_18

/* Kích thước giao dịch lớn nhất bus cần hỗ trợ (byte) - hiện chỉ VS1053 dùng bus này
   (VS1053_CHUNK_SIZE = 32, xem vs1053.h). Nếu sau này thêm device khác cần gửi/nhận khối lớn
   hơn, tăng giá trị này lên (không giảm - device hiện tại vẫn cần đủ 32) */
#define SPI_MAX_TRANSFER_VS1053_CHUNK_SIZE   32U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Cấu hình bus SPI dùng chung cho toàn hệ thống - định nghĩa (cấp phát thật) trong
   spi_config.c, Spi_Init() (driver/spi/spi.c) chỉ lấy địa chỉ dùng lại thay vì tự khai báo
   biến local. */
extern spi_bus_config_t gSpiBusConfig;

#endif /* SPI_CONFIG_H */
