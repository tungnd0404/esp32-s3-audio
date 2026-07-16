#ifndef SDMMC_H
#define SDMMC_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Sdmmc_Init
 * Chuẩn bị slot SDMMC theo chân/độ rộng bus cấu hình sẵn (SDMMC_CLK/CMD/D0_PIN,
 * SDMMC_BUS_WIDTH - xem sdmmc_config.h), ghi vào gSdmmcSlotConfig (định nghĩa trong
 * sdmmc_config.c) để Sdmmc_Mount() dùng lại sau này - gSdmmcHost (cùng file) đã có giá trị
 * mặc định đúng ngay từ lúc khai báo, hàm này không cần đụng vào. Tách khỏi Sdmmc_Mount() để
 * gọi được từ app_main() TRƯỚC KHI tạo Sdcard_Task - cùng nguyên tắc "hạ tầng dùng chung
 * chuẩn bị trước, task chỉ dùng lại" với Spi_Init()/I2c_Init()/Gpio_Init() (xem
 * spi.h/i2c.h/gpio.h). Gọi đúng 1 lần.
 * @param
 * @return ESP_OK luôn (chỉ gán giá trị struct trong RAM, chưa đụng phần cứng thật - ESP-IDF
 *         gộp host_init/slot_init vào bên trong esp_vfs_fat_sdmmc_mount(), xem Sdmmc_Mount())
 */
esp_err_t Sdmmc_Init(void);

/**
 * @brief Sdmmc_Mount
 * Mount thẻ SD thành filesystem FAT tại mountPoint, dùng host/slot config đã chuẩn bị sẵn
 * từ Sdmmc_Init() (YÊU CẦU Sdmmc_Init() đã gọi thành công từ trước, thường trong
 * app_main()). Gọi bởi Sdcard_Task lúc khởi động (xem Sdcard_Mount() trong task/sdcard.c).
 *
 * Tách riêng khỏi Sdcard_Mount() để phần "biết dùng SDMMC" nằm gọn 1 chỗ, không lẫn với
 * logic chung của module Sdcard (đường dẫn mount, cấu hình FAT) - nếu sau này board đổi
 * sang giao tiếp thẻ SD qua SPI/QSPI thay vì SDMMC, chỉ cần viết 1 driver mới (vd
 * driver/sdspi/sdspi.c) cùng chữ ký Sdmmc_Init()/Sdmmc_Mount(), Sdcard_Mount() chỉ cần đổi
 * tên hàm gọi, không phải viết lại phần còn lại của module.
 * @param mountPoint: đường dẫn VFS mount thẻ vào (vd "/sdcard", xem SDCARD_MOUNT_POINT trong
 *        sdcard_config.h)
 * @param pMountConfig: cấu hình mount FAT (format_if_mount_failed/max_files/
 *        allocation_unit_size) - bus-agnostic, bên gọi tự dựng từ sdcard_config.h
 * @param ppCard: [out] con trỏ nhận thông tin thẻ SD sau khi mount thành công
 * @return ESP_OK nếu mount thành công, mã lỗi esp_err_t khác nếu thất bại (vd Sdmmc_Init()
 *         chưa được gọi, thẻ không có/đấu sai dây)
 */
esp_err_t Sdmmc_Mount(const char *mountPoint, const esp_vfs_fat_sdmmc_mount_config_t *pMountConfig,
                      sdmmc_card_t **ppCard);

#endif /* SDMMC_H */
