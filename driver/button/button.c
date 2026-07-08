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
#define DOUBLE_CLICK_TIME       pdMS_TO_TICKS(300)

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

 /* trạng thái nút bấm */
typedef enum {
    BTN_EVENT_NEXT,
    BTN_EVENT_PREV,
    BTN_EVENT_PLAY,
    BTN_EVENT_MAX
} Button_EventType_e;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Array cho Timestamp chống rung */
volatile uint32_t gau32LastTickTime[BTN_EVENT_MAX] = 0;

/* ===================================================
 *  ISR
 * =================================================== */
/**
 * @brief Button_NextISR
 * @param
 * @return 
 */
/* ---------------------------------------------------------
ISR: NÚT NEXT
--------------------------------------------------------- */
void IRAM_ATTR Button_NextISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();
    Button_EventType_e leButtonEvent;

    if (lnow - gau32LastTickTime[BTN_EVENT_NEXT] > DEBOUNCE_TICKS)
    {
        leButtonEvent = BTN_EVENT_NEXT;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(xPlayerManagerTaskHandle, BTN_EVENT_NEXT, eSetValueWithOverwrite, NULL);

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
/* ---------------------------------------------------------
ISR: NÚT PREV
--------------------------------------------------------- */
void IRAM_ATTR Button_PrevISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();
    Button_EventType_e leButtonEvent;

    if (lu32Now - gau32LastTickTime[BTN_EVENT_PREV] > DEBOUNCE_TICKS)
    {
        leButtonEvent = BTN_EVENT_PREV;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(xPlayerManagerTaskHandle, EVENT_PREV, eSetValueWithOverwrite, NULL);

        if (xHigherPriorityTaskWoken) 
        {
            portYIELD_FROM_ISR();
        }
    }
    gau32LastTickTime[BTN_EVENT_PREV] = lu32Now;
}

/**
 * @brief button_playISR
 * @param
 * @return 
 */
/* ---------------------------------------------------------
ISR: NÚT PLAY
--------------------------------------------------------- */
void IRAM_ATTR button_playISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR();

    if (now - last_tick_play > DEBOUNCE_TICKS)
    {
        button_event evt = EVENT_PLAY;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xTaskNotifyFromISR(player_manager_task_handle, EVENT_PLAY, eSetValueWithOverwrite, NULL);

        if (xHigherPriorityTaskWoken) 
        {
            portYIELD_FROM_ISR();
        }
    }
    last_tick_play = now;
}

/* ==================== API công khai ==================== */
/**
 * @brief Khởi tạo button
 * @param
 * @return 
 */
void button_init()
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

    /* config GPIO */
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    /* đăng ký ngắt */
    gpio_isr_handler_add(BTN_NEXT, button_nextISR, NULL);
    gpio_isr_handler_add(BTN_PREV, button_prevISR, NULL);
    gpio_isr_handler_add(BTN_PLAY, button_playISR, NULL);
}

