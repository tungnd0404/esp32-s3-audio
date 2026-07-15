#ifndef MP3_H
#define MP3_H

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

/* Kích thước xMp3RingBuffer (byte) - vùng đệm dữ liệu mp3 thô, đọc trước từ thẻ SD bởi
   Sdcard_Task (owner duy nhất của thẻ SD) rồi Mp3_Task rút ra để gửi cho VS1053. 4KB đủ
   đệm trước vài trăm ms nhạc (tuỳ bitrate), trong khi Sdcard_Task nạp lại đầy mỗi ~50ms
   (xem Sdcard_LoadSong trong sdcard.c) - dư sức bù tốc độ VS1053 tiêu thụ. */
#define MP3_RING_BUFFER_SIZE   4096U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Mp3_Task */
extern TaskHandle_t xMp3TaskHandle;

/* Hàng đợi lệnh dùng chung tới Mp3_Task (phần tử kiểu Srm_Message_s), do Mp3_Init() tạo
   (Mp3_Task là owner) */
extern QueueHandle_t xMp3CommandQueue;

/* Ring buffer chứa dữ liệu thô của file mp3 bài đang phát, tạo trong Mp3_Init(). Sdcard_Task
   là bên GHI DUY NHẤT (owner của thẻ SD, tự mở file mp3 và nạp liên tục - xem
   Sdcard_LoadSong trong sdcard.c); Mp3_Task là bên ĐỌC DUY NHẤT (Mp3_StreamSong
   trong mp3.c). Ring buffer FreeRTOS đã tự an toàn cho đúng 1 task ghi + 1 task đọc đồng
   thời, không cần thêm mutex - đây chính là lý do đổi từ việc Mp3_Task tự fopen/fread thẳng
   trên thẻ SD (vi phạm kiến trúc Owner Task, xem srm.h) sang mô hình producer/consumer này. */
extern RingbufHandle_t xMp3RingBuffer;

/* true = Sdcard_Task đã đọc hết file mp3 hiện tại (fread trả về 0) và sẽ không ghi thêm gì
   vào xMp3RingBuffer nữa cho tới bài kế tiếp. Mp3_Task dựa vào cờ này để phân biệt "ring
   buffer tạm thời rỗng, đợi Sdcard_Task nạp thêm" (cờ còn false) với "đã phát hết bài, dừng
   hẳn" (cờ true VÀ ring buffer đã đọc cạn) - xem Mp3_StreamSong.
   volatile vì được Sdcard_Task ghi và Mp3_Task đọc liên tục trong 2 task khác nhau, ngoài cơ
   chế task notification - cùng lý do gsPlayerContext.playbackState volatile (player_manager.h) */
extern volatile bool gbMp3StreamEof;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào, và
 * xMp3RingBuffer để Sdcard_Task nạp dữ liệu mp3 thô vào cho Mp3_Task rút ra phát.
 * Gọi trước khi tạo Mp3_Task VÀ trước Sdcard_Init()/Sdcard_Task (xem audio.c).
 * @param
 * @return
 */
void Mp3_Init(void);

/**
 * @brief Mp3_Task
 * Task owner duy nhất của thiết bị VS1053 - stream file mp3 của bài đang phát ra loa, và
 * là nơi duy nhất được phép gọi API vs1053_* (kiến trúc Owner Task, xem srm.h).
 * Cùng khuôn thuật toán với Oled_Task/Sdcard_Task để các task trong hệ thống đọc thống
 * nhất (xem mp3.c để biết chi tiết từng hàm nội bộ).
 * @param pvParameters
 * @return
 */
void Mp3_Task(void *pvParameters);

#endif /* MP3_H */
