#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Debounce delay (ms)
#define DEBOUNCE_MS             80
#define DEBOUNCE_TICKS          (DEBOUNCE_MS / portTICK_PERIOD_MS)
#define DOUBLE_CLICK_TIME       pdMS_TO_TICKS(300)

#define SYSTEM_PLAYING_NEW      (1 << 0)
#define SYSTEM_PLAYING          (1 << 1)
#define SYSTEM_PAUSING          (1 << 2)
#define SYSTEM_NEXT             (1 << 3)
#define SYSTEM_PREV             (1 << 4)

// Sự kiện nút
typedef enum { 
    STATE_MENU,
    STATE_PLAYING
} state_main;

typedef enum {
    STATE_UP,
    STATE_DOWN,
    STATE_PLAY_NEW,
    STATE_PLAY,
    STATE_PAUSE,
    STATE_NEXT,
    STATE_PREV,
    STATE_IDLE
} state_button;

typedef enum {
    EVENT_NEXT,
    EVENT_PREV,
    EVENT_PLAY
} button_event;

extern volatile state_main stateMain;
extern volatile state_button stateButton;

// Khởi tạo nút bấm
void button_init();
void button_task(void *arg);
// ISR của từng nút
void IRAM_ATTR button_nextISR(void *arg);
void IRAM_ATTR button_prevISR(void *arg);
void IRAM_ATTR button_playISR(void *arg);
//extern volatile button_event buttonEvent = 0;
extern QueueHandle_t state_button_event_queue;
extern EventGroupHandle_t system_event_group;

#endif