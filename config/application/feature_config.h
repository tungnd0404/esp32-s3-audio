#ifndef FEATURE_CONFIG_H
#define FEATURE_CONFIG_H

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Bật/tắt các printf() debug rải rác trong Sdcard_Task/PlayerManager_Task (xem task/sdcard.c,
   task/player_manager.c) - comment dòng #define dưới đây để tắt hết trong build release.
   Đổi tên từ DEVELOPER_CONFIGURATION (config.h cũ) sang tên mô tả đúng NÓ LÀM GÌ (bật debug
   printf) thay vì ai dùng nó, tránh tên chung chung */
#define DEBUG_PRINTF_ENABLED

#endif /* FEATURE_CONFIG_H */
