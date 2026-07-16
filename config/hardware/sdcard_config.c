/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "sdcard_config.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) cấu hình mount FAT dùng chung cho thẻ SD - khai báo extern
   trong sdcard_config.h. Bus-agnostic: driver nào được chọn qua SDCARD_INTERFACE cũng nhận
   đúng con trỏ tới struct này (xem Sdcard_Mount() trong task/sdcard.c). */
esp_vfs_fat_sdmmc_mount_config_t gSdcardMountConfig =
{
    .format_if_mount_failed = SDCARD_FORMAT_IF_MOUNT_FAILED,
    .max_files = SDCARD_MAX_OPEN_FILES,
    .allocation_unit_size = SDCARD_FAT_ALLOCATION_UNIT_SIZE,
};
