#ifndef OLED_H
#define OLED_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "ssd1306.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
extern TaskHandle_t xOledTaskHandle;

/* Hàng đợi lệnh dùng chung tới Oled_Task (phần tử kiểu Srm_Message_s), do Oled_Init() tạo
   (Oled_Task là owner duy nhất của màn hình SSD1306 - kiến trúc Owner Task, xem srm.h).
   Hiện CHƯA có lệnh nào (Srm_CommandType_e) thật sự dùng tới hạ tầng này, vì chưa có module
   nào khác cần đụng vào SSD1306 (khác với VS1053/double buffer, vốn được nhiều task chia sẻ)
   - dựng sẵn hạ tầng owner (queue + Oled_HandleCommand + Oled_ServicePendingCommand trong
   oled.c) để nếu sau này có nhu cầu, chỉ cần thêm 1 case OLED_CMD_* + 1 hàm Srm_Oled<Lệnh>()
   gửi, không phải sửa lại cấu trúc Oled_Task */
extern QueueHandle_t xOledCommandQueue;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_Init
 * Khởi tạo màn hình OLED (I2C + SSD1306), đăng ký luôn buffer đích nhận frame animation cho
 * DoubleBuffer_GetFrame() (xem double_buffer.h)
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Oled_Init(SSD1306_t *dev);

/**
 * @brief Oled_Task
 * Task điều khiển màn hình OLED, nhận PlayerManager_ButtonStateType_e từ PlayerManager_Task
 * qua task notification (dùng chung thẳng enum này, không cần thêm 1 enum sự kiện riêng cho OLED)
 * @param pvParameters: con trỏ SSD1306_t* của màn hình, truyền vào lúc tạo task
 * @return
 */
void Oled_Task(void *pvParameters);

#endif /* OLED_H */
