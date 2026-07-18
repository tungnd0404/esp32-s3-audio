/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "button.h"
#include "button_config.h"
#include "gpio_config.h"
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
void Button_NextISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_NEXT] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        /* Dùng eSetBits (không phải eSetValueWithOverwrite) - 3 ISR Next/Prev/Play đang dùng
           CHUNG 1 giá trị notification của xPlayerManagerTaskHandle làm hộp thư.
           eSetValueWithOverwrite trước đây khiến 2 nút KHÁC NHAU bấm gần như đồng thời (trước
           khi PlayerManager_Task kịp thức dậy đọc giá trị đầu) làm MẤT HẲN 1 lần bấm - giá trị
           sau ghi đè giá trị trước. eSetBits gộp (OR) các bit lại thay vì ghi đè nên 2 sự kiện
           khác bit luôn cùng tồn tại tới khi PlayerManager_Task xử lý, không còn rơi mất - xem
           PlayerManager_Task (player_manager.c) xử lý từng bit độc lập thay vì switch trên 1
           giá trị duy nhất. Bit gửi đi suy trực tiếp từ Button_EventType_e (1U << BTN_EVENT_xxx)
           thay vì định nghĩa thêm 1 bộ macro bitmask riêng - tránh 2 bộ tên cùng đại diện cho
           đúng 3 nút NEXT/PREV/PLAY. */
        xTaskNotifyFromISR(xPlayerManagerTaskHandle, (1U << BTN_EVENT_NEXT), eSetBits, &xHigherPriorityTaskWoken);

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
void Button_PrevISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_PREV] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        /* eSetBits - xem giải thích đầy đủ trong Button_NextISR() ở trên */
        xTaskNotifyFromISR(xPlayerManagerTaskHandle, (1U << BTN_EVENT_PREV), eSetBits, &xHigherPriorityTaskWoken);

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
void Button_PlayISR(void *arg)
{
    uint32_t lu32Now = xTaskGetTickCountFromISR();

    /* Chống rung nút bấm */
    if (lu32Now - gau32LastTickTime[BTN_EVENT_PLAY] > DEBOUNCE_TICKS)
    {
        /* Cờ báo có task ưu tiên cao hơn được đánh thức sau khi notify hay không */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        /* eSetBits - xem giải thích đầy đủ trong Button_NextISR() ở trên */
        xTaskNotifyFromISR(xPlayerManagerTaskHandle, (1U << BTN_EVENT_PLAY), eSetBits, &xHigherPriorityTaskWoken);

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
void Button_Init(void)
{
    /* Config GPIO - dùng gButtonGpioConfig (định nghĩa trong gpio_config.c) thay vì tự
       khai báo/liệt kê pin_bit_mask ở đây, xem gpio_config.h/button_config.h */
    ESP_ERROR_CHECK(gpio_config(&gButtonGpioConfig));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    /* Đăng ký ngắt */
    gpio_isr_handler_add(BUTTON_NEXT_PIN, Button_NextISR, NULL);
    gpio_isr_handler_add(BUTTON_PREV_PIN, Button_PrevISR, NULL);
    gpio_isr_handler_add(BUTTON_PLAY_PIN, Button_PlayISR, NULL);
}

