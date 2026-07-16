#ifndef SDMMC_CONFIG_H
#define SDMMC_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Cấu hình RIÊNG cho giao diện SDMMC (chỉ có ý nghĩa khi SDCARD_INTERFACE ==
   SDCARD_INTERFACE_SDMMC, xem sdcard_config.h) - driver/sdmmc/sdmmc.c là nơi DUY NHẤT đọc
   các macro này, task/sdcard.c không đụng trực tiếp vào chân/độ rộng bus SDMMC nữa. */

/* --- Chân SDMMC vật lý - đổi tại đây nếu đấu dây khác board --- */
#define SDMMC_CLK_PIN   GPIO_NUM_39
#define SDMMC_CMD_PIN   GPIO_NUM_38
#define SDMMC_D0_PIN    GPIO_NUM_40

/* Độ rộng bus SDMMC (bit) - 1 = chỉ dùng CLK/CMD/D0 (không dùng D1/D2/D3), phù hợp board
   hiện tại chỉ đấu 3 chân data. Đổi thành 4 nếu board đấu đủ D1/D2/D3 để chạy tốc độ cao hơn
   (xem Sdmmc_Mount() trong driver/sdmmc/sdmmc.c) */
#define SDMMC_BUS_WIDTH   1

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Host + slot SDMMC dùng chung - định nghĩa (cấp phát thật) trong sdmmc_config.c.
   gSdmmcHost đã đủ dùng ngay với giá trị mặc định (SDMMC_HOST_DEFAULT()), không cần chỉnh gì
   thêm. gSdmmcSlotConfig cần Sdmmc_Init() (driver/sdmmc/sdmmc.c) gán lại 1 lần lúc chạy (áp
   SDMMC_SLOT_CONFIG_DEFAULT() rồi ghi đè width/clk/cmd/d0 theo board) trước khi
   Sdmmc_Mount() dùng tới - xem sdmmc_config.c để biết vì sao không thể khởi tạo đầy đủ ngay
   lúc khai báo như gSdmmcHost. */
extern sdmmc_host_t gSdmmcHost;
extern sdmmc_slot_config_t gSdmmcSlotConfig;

#endif /* SDMMC_CONFIG_H */
