#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "config.h"
/* #include "freertos/FreeRTOS.h"
#include "freertos/task.h"
 */




/* ==================== API công khai ==================== */
/* Khởi tạo nút bấm */
void button_init();

/* ==================== ISR ==================== */
/* ISR */
void IRAM_ATTR button_nextISR(void *arg);
void IRAM_ATTR button_prevISR(void *arg);
void IRAM_ATTR button_playISR(void *arg);

/* ==================== Global variable =================== */
extern QueueHandle_t state_button_event_queue;

#endif