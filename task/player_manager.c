/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "player_manager.h"
#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "oled.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* trạng thái hệ thống */
typedef enum { 
    MAIN_STATE_MENU,
    MAIN_STATE_PLAYING
} PlayerManager_MainStateType_e;

/* trạng thái nút bấm */
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

/* state system */
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
/* queue khởi tạo */
QueueHandle_t xOledQueue;
QueueHandle_t xMp3Queue;

/* task handler Player Manager*/
TaskHandle_t xPlayerManagerTaskHandle = NULL;

/* state system */
PlayerManager__PlayerContextType_s gPlayerContext;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */
/**
 * @brief Update cursor position
 * @param cursor: vị trí hiện tại
 * @param totalItem: tổng số item
 * @param direction: hướng di chuyển (BUTTON_STATE_NEXT: next, BUTTON_STATE_PREV: prev)
 * @return cursor đã được update
 */
static uint32_t PlayerManager_Update_Cursor(uint32_t cursor, uint32_t totalItem, uint8_t direction)
{
    /* Check tổng số item */
    if (totalItem == 0)
    {
        return 0;
    }
    /* Update cursor position */
    if (direction == BUTTON_STATE_NEXT) /* Next */
    {
        cursor = (cursor + 1U) % totalItem;
    }
    else /* Prev */
    {
        cursor = (cursor + totalItem - 1U) % totalItem;
    }

    return cursor;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief PlayerManager_Init
 * Khởi tạo player manager
 * @param
 * @return 
 */
void PlayerManager_Init()
{
    /* khởi tạo state system */
    gPlayerContext.mainState = MAIN_STATE_MENU;
    gPlayerContext.buttonState = BUTTON_STATE_IDLE;

    /* khởi tạo double buffer, ring buffer */
    /* double_buffer_init(); */

    /* khởi tạo event, queue */
    
}

/**
 * @brief PlayerManager_Task
 * Main controller task của hệ thống.
 * Nhiệm vụ:
 * - Nhận command từ button/event queue
 * - Quản lý state toàn hệ thống:
 *     + MENU
 *     + PLAYING
 *     + PAUSE
 * - Điều phối các task khác:
 * - Xử lý:
 *     + next/prev song
 *     + play/pause
 *     + double click
 *     + playlist/menu navigation
 * - Gửi event queue đồng bộ hệ thống
 * @return
 */
void PlayerManager_Task(void *arg)
{
    uint32_t lbutton_evt;
    while (1) 
    {
        /* Đợi notification từ button */
        if (xTaskNotifyWait(0, UINT32_MAX, &button_evt, portMAX_DELAY) == pdTRUE)
        {
            switch (lbutton_evt) 
            {
                case EVENT_NEXT:
                    /* xử lý next */
                    if (gPlayerContext.mainState == STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON UP\n");
                        #endif
                        /* update cursor */
                        gPlayerContext.cursor = PlayerManager_Update_Cursor((uint32_t)gPlayerContext.cursor, (uint32_t)gPlayerContext.totalSong, BUTTON_STATE_PREV);
                        /* Update trạng thái button */
                        gPlayerContext.buttonState = BUTTON_STATE_UP;
                        /* trigger notification */
                        xTaskNotifyGive(oled_taskHandle);
                    }
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON NEXT\n");
                        #endif
                        /* Update trạng thái button */
                        stateButton = STATE_NEXT;
                        /* update cursor */
                        cursor = update_cursor(cursor, song_count, +1);
                        /* update current song */
                        current_song = cursor;
                    }
                    break;
                case EVENT_PREV:
                    /* xử lý prev */
                    if (stateMain == STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON DOWN\n");
                        #endif
                        /* update cursor */
                        cursor = update_cursor(cursor, song_count, +1);
                        /* Update trạng thái button */
                        stateButton = STATE_DOWN;
                    }
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON PREV\n");
                        #endif
                        /* Update trạng thái button */
                        stateButton = STATE_PREV;
                        /* update cursor */
                        cursor = update_cursor(cursor, song_count, -1);
                        /* update current song */
                        current_song = cursor;
                    }
                    break;
                case EVENT_PLAY:
                {
                    TickType_t now = xTaskGetTickCount();
                    if (stateMain == STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON SELECT AND START PLAYING\n");
                        #endif
                        /* Update trạng thái Main */
                        stateMain = STATE_PLAYING;
                        /* Update trạng thái button */
                        stateButton = STATE_PLAY_NEW;
                        /* update current song */
                        current_song = cursor;
                    }
                    else if (stateMain == STATE_PLAYING)
                    {
                        /* CHECK DOUBLE CLICK */
                        if ((last_click_time != 0) && ((now - last_click_time) < DOUBLE_CLICK_TIME))
                        {
                            #if defined DEVELOPER_CONFIGURATION
                            printf("DOUBLE CLICK -> MENU\n");
                            #endif
                            /* Double click → về MENU */
                            /* Update trạng thái Main */
                            stateMain = STATE_MENU;
                            /* Update trạng thái button */
                            stateButton = STATE_IDLE;
                            /* Reset tránh lặp */
                            last_click_time = 0; 
                        }
                        else
                        {
                            /* Single click → play/pause */
                            #if defined DEVELOPER_CONFIGURATION
                            printf("SINGLE CLICK\n");
                            #endif
                            if (stateButton == STATE_PLAY)
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PAUSE\n");
                                #endif
                                /* Update trạng thái button */
                                stateButton = STATE_PAUSE;
                            }
                            else
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PLAY\n");
                                #endif
                                /* Update trạng thái button */
                                stateButton = STATE_PLAY;
                            }
                            /* Update thời gian click cuối cùng */
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