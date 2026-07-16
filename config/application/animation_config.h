#ifndef ANIMATION_CONFIG_H
#define ANIMATION_CONFIG_H

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số frame animation trên giây - dùng chung bởi Oled_Task (oled.c, tính nhịp nghỉ giữa các
   frame khi phát animation) và SyncFrame (driver/buffer/sync_frame.c, quy đổi thời gian đã
   giải mã sang chỉ số frame tương ứng) - đổi tại đây để đồng bộ cả 2 nơi cùng lúc */
#define ANIMATION_FPS    15

#endif /* ANIMATION_CONFIG_H */
