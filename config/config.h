#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* config.h CHỈ đóng vai trò include lại toàn bộ config con bên dưới để tiện xem tổng quan
   toàn hệ thống ở 1 chỗ (vd người mới đọc code) - driver/task cụ thể KHÔNG include file này,
   mà include thẳng đúng 1 hoặc vài file con mình thực sự cần (vd max98357a.c chỉ include
   "i2s_config.h", không include "config.h"), để dependency giữa các module luôn tường
   minh, không vô tình kéo theo cấu hình không liên quan tới mình. Xem README/hướng dẫn
   module hoá config để biết quy ước thêm file config mới. */

/*------------------------------------------------------------
                    HARDWARE CONFIGURATION
------------------------------------------------------------*/
#include "hardware/button_config.h"
#include "hardware/ssd1306_config.h"
#include "hardware/i2s_config.h"
#include "hardware/sdcard_config.h"
#include "hardware/sdmmc_config.h"
#include "hardware/i2c_config.h"

/*------------------------------------------------------------
                    APPLICATION CONFIGURATION
------------------------------------------------------------*/
#include "application/animation_config.h"
#include "application/feature_config.h"

#ifdef __cplusplus
}
#endif

#endif // CONFIG_H
