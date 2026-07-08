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
 *  GLOBAL VARIABLES
 * =================================================== */

extern TaskHandle_t xPlayerManagerTaskHandle;

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
