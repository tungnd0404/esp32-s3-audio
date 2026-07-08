/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

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


/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */
extern TaskHandle_t player_manager_task_handle;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */
/**
 * @brief player_manager_task
 * @param
 * @return 
 */
void player_manager_task(void *arg);

#endif /* PLAYER_MANAGER_H */