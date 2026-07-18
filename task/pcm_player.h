#ifndef PCM_PLAYER_H
#define PCM_PLAYER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "srm.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Kích thước xPcmRingBuffer (byte) - vùng đệm PCM thô, đọc trước từ thẻ SD bởi Sdcard_Task
   (owner duy nhất của thẻ SD) rồi Pcm_Task rút ra ghi cho I2S. PCM 16-bit stereo 44.1kHz cần
   ~172KB/s (gấp ~10 lần MP3 128kbps trước đây) nên buffer này lớn hơn hẳn xMp3RingBuffer cũ
   (4KB) - 64KB đủ đệm trước ~370ms nhạc, dư sức chống underrun khi Sdcard_Task/thẻ SD chậm
   thoáng qua, vẫn nhỏ so với RAM còn trống của ESP32-S3 (~275KB DRAM lúc boot) */
#define PCM_RING_BUFFER_SIZE   65536U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Pcm_Task */
extern TaskHandle_t xPcmTaskHandle;

/* Hàng đợi lệnh dùng chung tới Pcm_Task (phần tử kiểu Srm_Message_s), do Pcm_Init() tạo
   (Pcm_Task là owner) */
extern QueueHandle_t xPcmCommandQueue;

/* Ring buffer chứa dữ liệu PCM thô của file .pcm bài đang phát, tạo trong Pcm_Init().
   Sdcard_Task là bên GHI DUY NHẤT (owner của thẻ SD, tự mở file .pcm và nạp liên tục - xem
   Sdcard_LoadSong trong sdcard.c); Pcm_Task là bên ĐỌC DUY NHẤT (Pcm_StreamSong trong
   pcm_player.c). Ring buffer FreeRTOS đã tự an toàn cho đúng 1 task ghi + 1 task đọc đồng
   thời, không cần thêm mutex (xem hợp đồng an toàn luồng trong ring_buffer.h) */
extern RingbufHandle_t xPcmRingBuffer;

/* true = Sdcard_Task đã đọc hết file .pcm hiện tại (fread trả về 0) và sẽ không ghi thêm gì
   vào xPcmRingBuffer nữa cho tới bài kế tiếp. Pcm_Task dựa vào cờ này để phân biệt "ring
   buffer tạm thời rỗng, đợi Sdcard_Task nạp thêm" (cờ còn false) với "đã phát hết bài, dừng
   hẳn" (cờ true VÀ ring buffer đã đọc cạn) - xem Pcm_StreamSong.
   volatile vì được Sdcard_Task ghi và Pcm_Task đọc liên tục trong 2 task khác nhau, ngoài cơ
   chế task notification */
extern volatile bool gbPcmStreamEof;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Pcm_Init
 * Khởi tạo module Pcm: tạo xPcmCommandQueue để các module khác (sync_frame.c qua SRM) gửi
 * lệnh vào, và xPcmRingBuffer để Sdcard_Task nạp dữ liệu PCM thô vào cho Pcm_Task rút ra phát.
 * Gọi bởi chính Pcm_Task lúc khởi động (đầu Pcm_Task, trước Max98357a_Init()), không gọi từ
 * app_main() - an toàn dù Sdcard_Task (task khác, core khác) khởi động song song, vì
 * Sdcard_Task chỉ ghi vào xPcmRingBuffer lúc thực sự nạp bài (Sdcard_LoadSong), luôn cần
 * người dùng bấm nút trước - độ trễ phản xạ người dùng dư sức lớn hơn thời gian hàm này chạy
 * xong.
 * @param
 * @return
 */
void Pcm_Init(void);

/**
 * @brief Pcm_Task
 * Task owner duy nhất của kênh I2S/MAX98357A - stream file PCM của bài đang phát ra loa, và
 * là nơi duy nhất được phép gọi API Max98357a_* (kiến trúc Owner Task, xem srm.h). Cùng khuôn
 * thuật toán với Oled_Task/Sdcard_Task để các task trong hệ thống đọc thống nhất (xem
 * pcm_player.c để biết chi tiết từng hàm nội bộ). Thay thế hoàn toàn Mp3_Task/vs1053.c cũ -
 * KHÔNG còn khái niệm "huỷ/xả FIFO chip giải mã" khi đổi bài (VS1053 cần vs1053_cancel_song(),
 * I2S/PCM chỉ cần reset con trỏ đọc + reset played_samples, xem case BTN_STATE_NEXT/PREV/
 * PLAY_NEW), đơn giản hoá đáng kể so với trước.
 * @param pvParameters
 * @return
 */
void Pcm_Task(void *pvParameters);

#endif /* PCM_PLAYER_H */
