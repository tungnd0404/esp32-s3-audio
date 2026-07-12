#ifndef MP3_H
#define MP3_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "srm.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Mp3_Task */
extern TaskHandle_t xMp3TaskHandle;

/* Hàng đợi lệnh dùng chung tới Mp3_Task (phần tử kiểu Srm_Message_s), do Mp3_Init() tạo
   (Mp3_Task là owner) */
extern QueueHandle_t xMp3CommandQueue;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào.
 * Gọi trước khi tạo Mp3_Task.
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
