#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

#define BTN1_BIT   BIT0
#define BTN2_BIT   BIT1
#define BTN3_BIT   BIT2

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* Trạng thái giao diện chính */
typedef enum {
    MAIN_STATE_MENU,
    MAIN_STATE_PLAYING
} PlayerManager_MainStateType_e;

/* Trạng thái nút bấm */
typedef enum {
    BTN_STATE_UP,
    BTN_STATE_DOWN,
    BTN_STATE_PLAY_NEW,
    BTN_STATE_PLAY,
    BTN_STATE_PAUSE,
    BTN_STATE_NEXT,
    BTN_STATE_PREV,
    BTN_STATE_IDLE
} PlayerManager_ButtonStateType_e;

/* State toàn hệ thống player */
typedef struct
{
    PlayerManager_MainStateType_e mainState;
    PlayerManager_ButtonStateType_e buttonState;
    uint32_t cursor;
    uint32_t currentSong;
    uint32_t totalSong;
} PlayerManager__PlayerContextType_s;

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

extern TaskHandle_t xPlayerManagerTaskHandle;

/* State toàn hệ thống player, dùng chung cho player_manager/oled/menu */
extern PlayerManager__PlayerContextType_s gsPlayerContext;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief PlayerManager_Init
 * Khởi tạo player manager
 * @param
 * @return
 */
void PlayerManager_Init(void);

/**
 * @brief PlayerManager_Task
 * @param
 * @return
 */
void PlayerManager_Task(void *arg);

#endif /* PLAYER_MANAGER_H */
