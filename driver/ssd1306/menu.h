#ifndef MENU_H
#define MENU_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include "ssd1306.h"
#include "sdcard.h"
#include "driver/gpio.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Menu_Draw
 * Vẽ danh sách bài hát lên màn hình OLED, đánh dấu vị trí cursor hiện tại (gsPlayerContext.cursor)
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Menu_Draw(SSD1306_t *dev);

/**
 * @brief Menu_UpdateScroll
 * Cập nhật lại gi32StartIndex để cursor hiện tại luôn nằm trong vùng hiển thị
 * @param
 * @return
 */
void Menu_UpdateScroll(void);

#endif /* MENU_H */
