#ifndef VS1053_CONFIG_H
#define VS1053_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* --- Chân kết nối vật lý VS1053 - đổi tại đây nếu đấu dây khác board --- */
#define VS1053_XCS_PIN      GPIO_NUM_5     /* Chip select cho lệnh SCI (XCS) */
#define VS1053_XDCS_PIN     GPIO_NUM_26    /* Chip select cho dữ liệu SDI (XDCS) */
#define VS1053_DREQ_PIN     GPIO_NUM_27    /* Data request - chip kéo cao khi sẵn sàng nhận lệnh/data */
#define VS1053_RESET_PIN    GPIO_NUM_32    /* Reset phần cứng, tích cực mức thấp */

/* --- Tốc độ SPI của VS1053 (Hz) - đổi tại đây nếu cần tinh chỉnh, xem vs1053.c --- */
/* Tốc độ lúc mới reset/khởi tạo - chậm, an toàn vì lúc này VS1053 vẫn đang chạy clock nội bộ
   mặc định (chưa nhân clock qua SCI_CLOCKF), SPI nhanh hơn khả năng chip xử lý sẽ mất dữ liệu */
#define VS1053_SPI_INIT_CLOCK_HZ    200000U
/* Tốc độ lúc chạy bình thường (sau khi đã cấu hình SCI_CLOCKF = 0x6000, nhân clock lên
   3.0x = 12.288MHz) - nâng lên ở cuối vs1053_init() */
#define VS1053_SPI_RUN_CLOCK_HZ     4000000U

#endif /* VS1053_CONFIG_H */
