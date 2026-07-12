#ifndef SYNC_FRAME_H
#define SYNC_FRAME_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mp3.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief SyncFrame_Init
 * Khởi tạo bộ đồng bộ animation theo thời gian giải mã mp3. Gọi 1 lần khi bắt đầu phát
 * 1 bài mới, trước khi dùng SyncFrame_GetFrameIndex()
 * @param
 * @return
 */
void SyncFrame_Init(void);

/**
 * @brief SyncFrame_GetFrameIndex
 * Tính chỉ số frame animation cần hiển thị tại thời điểm hiện tại, đồng bộ theo thời gian
 * giải mã mp3 thực tế. Lấy decode time bằng cách gửi lệnh MP3_CMD_GET_DECODE_TIME cho
 * Mp3_Task xử lý (không gọi thẳng VS1053)
 * @param
 * @return chỉ số frame animation (bắt đầu từ 0)
 */
uint32_t SyncFrame_GetFrameIndex(void);

#endif /* SYNC_FRAME_H */
