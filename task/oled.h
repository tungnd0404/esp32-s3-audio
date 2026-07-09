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

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* Loại sự kiện gửi cho Oled_Task qua task notification */
typedef enum {
    OLED_EVENT_MENU_DRAW,   /* Cursor menu di chuyển -> vẽ lại menu */
    OLED_EVENT_SONG_CHANGED,  /* Bài hát thay đổi -> reset đồng bộ, load lại double buffer cho bài mới */
    OLED_EVENT_PLAY_PAUSE     /* Play/Pause đổi trạng thái, hoặc quay lại màn hình playing của bài đang phát -> không load lại bài, chỉ tiếp tục/tạm dừng animation */
} Oled_EventType_e;

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
extern TaskHandle_t xOledTaskHandle;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_Init
 * Khởi tạo màn hình OLED (I2C + SSD1306)
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Oled_Init(SSD1306_t *dev);

/**
 * @brief Oled_Task
 * Task điều khiển màn hình OLED, nhận Oled_EventType_e từ PlayerManager_Task qua task notification
 * @param pvParameters: con trỏ SSD1306_t* của màn hình, truyền vào lúc tạo task
 * @return
 */
void Oled_Task(void *pvParameters);

#endif /* OLED_H */
