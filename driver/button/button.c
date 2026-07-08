/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "button.h"
#include "player_manager.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

 /* Debounce delay (ms) */
#define DEBOUNCE_MS             80U
#define DEBOUNCE_TICKS          (DEBOUNCE_MS / portTICK_PERIOD_MS)

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Array cho Timestamp chống rung */
volatile uint32_t gau32LastTickTime[BTN_EVENT_MAX] = {0};

/* ===================================================
 *  ISR
 * =================================================== */
/**
 * @brief Button_NextISR
 * @param
 * @return 
 */
/* ===================================================
 *  ISR NEXT
 * =================================================== */
void IRAM_ATTR Button_NextISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_NEXT] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(xPlayerManagerTaskHandle, BTN_EVENT_NEXT, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

        if (xHigherPriorityTaskWoken) 
        {
            portYIELD_FROM_ISR();
        }
    }
    gau32LastTickTime[BTN_EVENT_NEXT] = lu32Now;
}

/**
 * @brief Button_NextISR
 * @param
 * @return 
 */
/* ===================================================
 *  ISR PREV
 * =================================================== */
void IRAM_ATTR Button_PrevISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_PREV] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(xPlayerManagerTaskHandle, BTN_EVENT_PREV, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

        if (xHigherPriorityTaskWoken) 
        {
            portYIELD_FROM_ISR();
        }
    }
    gau32LastTickTime[BTN_EVENT_PREV] = lu32Now;
}

/**
 * @brief Button_PlayISR
 * @param
 * @return 
 */
/* ===================================================
 *  ISR PLAY
 * =================================================== */
void IRAM_ATTR Button_PlayISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_PLAY] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(xPlayerManagerTaskHandle, BTN_EVENT_PLAY, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

        if (xHigherPriorityTaskWoken) 
        {
            portYIELD_FROM_ISR();
        }
    }
    gau32LastTickTime[BTN_EVENT_PLAY] = lu32Now;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */
/**
 * @brief Khởi tạo button
 * @param
 * @return 
 */
void Button_Init()
{
    /* khởi tạo config */
    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask =
            (1ULL << BTN_NEXT_PIN) |
            (1ULL << BTN_PREV_PIN) |
            (1ULL << BTN_PLAY_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    /* Config GPIO */
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    /* Đăng ký ngắt */
    gpio_isr_handler_add(BTN_NEXT_PIN, Button_NextISR, NULL);
    gpio_isr_handler_add(BTN_PREV_PIN, Button_PrevISR, NULL);
    gpio_isr_handler_add(BTN_PLAY_PIN, Button_PlayISR, NULL);
}

