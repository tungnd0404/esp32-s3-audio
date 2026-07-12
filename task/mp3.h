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
 * Task owner duy nhất của thiết bị VS1053. Nhận Srm_Message_s từ xMp3CommandQueue, dựa
 * vào cmdId (Srm_CommandType_e) để gọi API vs1053_* tương ứng, trả kết quả về bằng
 * Srm_Reply().
 * Thân hàm hiện để trống, sẽ triển khai sau (stream mp3 + xử lý command).
 * @param pvParameters
 * @return
 */
void Mp3_Task(void *pvParameters);

#endif /* MP3_H */
