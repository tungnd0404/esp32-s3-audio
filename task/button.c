#include "button.h"
#include "menu.h"

// Biến flag thông báo sự kiện cho main loop
//volatile int buttonEvent = 0;

volatile state_main stateMain = STATE_MENU;
volatile state_button stateButton = STATE_IDLE;

QueueHandle_t state_button_event_queue;
EventGroupHandle_t system_event_group;

// Timestamp chống rung
volatile uint32_t last_tick_next = 0;
volatile uint32_t last_tick_prev = 0;
volatile uint32_t last_tick_play = 0;

static TickType_t last_click_time = 0;
// ---------------------------------------------------------
// ISR: NÚT NEXT
// ---------------------------------------------------------
void IRAM_ATTR button_nextISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR();

    if (now - last_tick_next > DEBOUNCE_TICKS)
    {
        button_event evt = EVENT_NEXT;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xQueueSendFromISR(state_button_event_queue, &evt, &xHigherPriorityTaskWoken);
        

        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
    last_tick_next = now;
}

// ---------------------------------------------------------
// ISR: NÚT PREV
// ---------------------------------------------------------
void IRAM_ATTR button_prevISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR();

    if (now - last_tick_prev > DEBOUNCE_TICKS)
    {
        button_event evt = EVENT_PREV;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xQueueSendFromISR(state_button_event_queue, &evt, &xHigherPriorityTaskWoken);
        

        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
    last_tick_prev = now;
}

// ---------------------------------------------------------
// ISR: NÚT PLAY
// ---------------------------------------------------------
void IRAM_ATTR button_playISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR();

    if (now - last_tick_play > DEBOUNCE_TICKS)
    {
        button_event evt = EVENT_PLAY;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xQueueSendFromISR(state_button_event_queue, &evt, &xHigherPriorityTaskWoken);
        

        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
    last_tick_play = now;
}

// ---------------------------------------------------------
// KHỞI TẠO NÚT BẤM
// ---------------------------------------------------------
void button_init()
{
    system_event_group = xEventGroupCreate();
    state_button_event_queue = xQueueCreate(10, sizeof(button_event));
    if (state_button_event_queue == NULL)
        {
            printf("QUEUE ERROR");
        }
    
    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask =
            (1ULL << BTN_NEXT) |
            (1ULL << BTN_PREV) |
            (1ULL << BTN_PLAY),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    gpio_isr_handler_add(BTN_NEXT, button_nextISR, NULL);
    gpio_isr_handler_add(BTN_PREV, button_prevISR, NULL);
    gpio_isr_handler_add(BTN_PLAY, button_playISR, NULL);
}

static int update_cursor(int current, int total, int direction)
{
    // direction: +1 (NEXT), -1 (PREV)

    current = (current + direction + total) % total;

    return current;
}

void button_task(void *arg)
{
    button_event evt;
    while (1) {
        if (xQueueReceive(state_button_event_queue, &evt, portMAX_DELAY)) 
        {
            switch (evt) {
                case EVENT_NEXT:
                    // xử lý next
                    if (stateMain == STATE_MENU)
                    {
                        printf("BUTTON UP\n");
                        stateButton = STATE_UP;
                                        }
                    else
                    {
                        stateButton = STATE_NEXT;
                        printf("BUTTON NEXT\n");
                        /* update cursor */
                        cursor = update_cursor(cursor, song_count, +1);
                        /* set event */
                        xEventGroupSetBits(system_event_group, SYSTEM_NEXT);
                    }
                    break;
                case EVENT_PREV:
                    // xử lý next
                    if (stateMain == STATE_MENU)
                    {
                        stateButton = STATE_DOWN;
                        printf("BUTTON DOWN\n");
                    }
                    else
                    {
                        stateButton = STATE_PREV;
                        printf("BUTTON PREV\n");
                        /* update cursor */
                        cursor = update_cursor(cursor, song_count, -1);
                        /* set event */
                        xEventGroupSetBits(system_event_group, SYSTEM_PREV);
                    }
                    break;
                case EVENT_PLAY:
                {
                    TickType_t now = xTaskGetTickCount();
                    if (stateMain == STATE_MENU)
                    {
                        stateButton = STATE_PLAY_NEW;
                        printf("BUTTON SELECT\n");
                        stateMain = STATE_PLAYING;
                        printf("STATE PLAYING\n");
                        xEventGroupSetBits(system_event_group, SYSTEM_PLAYING_NEW);
                        printf("PLAY\n");
                    }
                    if (stateMain == STATE_PLAYING) // STATE_PLAYING
                    {
                        // CHECK DOUBLE CLICK
                        if ((now - last_click_time) < DOUBLE_CLICK_TIME)
                        {
                            // Double click → về MENU
                            stateMain = STATE_MENU;
                            stateButton = STATE_IDLE;
                        
                            printf("DOUBLE CLICK -> MENU\n");
                        
                            last_click_time = 0; // reset tránh lặp
                        }
                        else
                        {
                            // Single click → play/pause
                            if (stateButton == STATE_PLAY)
                            {
                                stateButton = STATE_PAUSE;
                                printf("PAUSE\n");
                            }
                            else
                            {
                                stateButton = STATE_PLAY;
                                printf("PLAY\n");
                            }
                        
                            last_click_time = now;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}