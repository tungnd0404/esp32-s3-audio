#ifndef BUTTON_H
#define BUTTON_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* Sự kiện nút bấm */
typedef enum {
    BTN_EVENT_NEXT,
    BTN_EVENT_PREV,
    BTN_EVENT_PLAY,
    BTN_EVENT_MAX
} Button_EventType_e;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Button_Init
 * Khởi tạo nút bấm
 * @param
 * @return
 */
void Button_Init();

/* ===================================================
 *  ISR
 * =================================================== */

void Button_NextISR(void *arg);
void Button_PrevISR(void *arg);
void Button_PlayISR(void *arg);

#endif /* BUTTON_H */
