#ifndef SDCARD_CONFIG_H
#define SDCARD_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "esp_vfs_fat.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Cấu hình CHUNG cho thẻ SD, KHÔNG phụ thuộc giao diện phần cứng thật sự dùng để giao tiếp
   (SDMMC/SPI/QSPI - xem SDCARD_INTERFACE bên dưới) - Sdcard_Mount() (task/sdcard.c) chỉ đọc
   các macro ở đây, phần biết chi tiết chân/bus nằm trong driver tương ứng (hiện tại:
   driver/sdmmc/sdmmc.c + sdmmc_config.h) */

/* Đường dẫn mount thẻ SD trong VFS - dùng chung bởi mọi nơi cần truy cập file trên thẻ
   (Sdcard_Mount()/Sdcard_ScanAndCreateDb()/SDCARD_DB_PATH, xem task/sdcard.c) */
#define SDCARD_MOUNT_POINT   "/sdcard"

/* Định dạng filesystem trên thẻ - hiện chỉ FAT32 được hỗ trợ (esp_vfs_fat_sdmmc_mount() của
   ESP-IDF). Để đây làm điểm mở rộng tường minh nếu sau này cần hỗ trợ thêm định dạng khác. */
#define SDCARD_FS_TYPE_FAT32   0
#define SDCARD_FS_TYPE         SDCARD_FS_TYPE_FAT32

/* true: tự format thẻ thành FAT32 nếu mount thất bại (XOÁ HẾT dữ liệu cũ trên thẻ) - để false
   để BÁO LỖI thay vì âm thầm xoá dữ liệu người dùng, đúng hành vi mong muốn cho máy nghe nhạc
   (thẻ chứa nhạc của người dùng, không phải bộ nhớ tạm dùng 1 lần) */
#define SDCARD_FORMAT_IF_MOUNT_FAILED   false

/* Số file tối đa có thể mở đồng thời qua VFS - hiện dùng tối đa 1 file mp3 + 1 file database
   cùng lúc (xem Sdcard_LoadSong()/Sdcard_ScanAndCreateDb()), để dư cho debug/mở rộng sau này */
#define SDCARD_MAX_OPEN_FILES   5

/* Kích thước cluster FAT filesystem (byte) - 16KB phù hợp thẻ SD dung lượng lớn (khớp cluster
   size mặc định của hầu hết thẻ SDHC/SDXC đã format sẵn từ nhà sản xuất) */
#define SDCARD_FAT_ALLOCATION_UNIT_SIZE   (16U * 1024U)

/* Giao diện phần cứng đang dùng để giao tiếp với thẻ SD - hiện chỉ SDMMC được triển khai
   (xem driver/sdmmc/sdmmc.c + sdmmc_config.h). Nếu sau này board đổi sang SD qua SPI/QSPI,
   viết driver mới cùng khuôn mẫu (vd driver/sdspi/ với Sdspi_Mount() cùng chữ ký
   Sdmmc_Mount()), đổi giá trị macro này, rồi đổi đúng 1 lời gọi trong Sdcard_Mount()
   (task/sdcard.c) - không cần sửa lại phần còn lại của module Sdcard */
#define SDCARD_INTERFACE_SDMMC   0
#define SDCARD_INTERFACE_SPI     1
#define SDCARD_INTERFACE         SDCARD_INTERFACE_SDMMC

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Cấu hình mount FAT dùng chung cho thẻ SD (bus-agnostic - kiểu esp_vfs_fat_sdmmc_mount_config_t
   của ESP-IDF thực chất dùng chung cho cả esp_vfs_fat_sdmmc_mount() lẫn esp_vfs_fat_sdspi_mount(),
   không riêng gì SDMMC dù tên type có chữ "sdmmc") - định nghĩa (cấp phát thật) trong
   sdcard_config.c, Sdcard_Mount() (task/sdcard.c) chỉ lấy địa chỉ dùng lại thay vì tự khai
   báo biến local. Đặt ở đây (không phải sdmmc_config.c) để giữ đúng ranh giới bus-agnostic -
   driver theo SDCARD_INTERFACE nào cũng dùng lại được đúng 1 struct này. */
extern esp_vfs_fat_sdmmc_mount_config_t gSdcardMountConfig;

#endif /* SDCARD_CONFIG_H */
