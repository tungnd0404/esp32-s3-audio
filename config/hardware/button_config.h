#ifndef BUTTON_CONFIG_H
#define BUTTON_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* --- Chân nút bấm vật lý - đổi tại đây nếu đấu dây khác board --- */
#define BUTTON_NEXT_PIN   GPIO_NUM_16
#define BUTTON_PREV_PIN   GPIO_NUM_17
#define BUTTON_PLAY_PIN   GPIO_NUM_18

#endif /* BUTTON_CONFIG_H */
