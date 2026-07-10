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
 * Task điều khiển màn hình OLED, nhận PlayerManager_ButtonStateType_e từ PlayerManager_Task
 * qua task notification (dùng chung thẳng enum này, không cần thêm 1 enum sự kiện riêng cho OLED)
 * @param pvParameters: con trỏ SSD1306_t* của màn hình, truyền vào lúc tạo task
 * @return
 */
void Oled_Task(void *pvParameters);

#endif /* OLED_H */
