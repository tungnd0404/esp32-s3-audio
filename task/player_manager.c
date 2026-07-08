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

/* Thời gian tối đa giữa 2 lần click để tính là double click */
#define DOUBLE_CLICK_TIME       pdMS_TO_TICKS(300)

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* trạng thái giao diện */
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
PlayerManager__PlayerContextType_s gsPlayerContext;

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
 * @param direction: hướng di chuyển (BTN_STATE_NEXT: next, BTN_STATE_PREV: prev)
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
    if (direction == BTN_STATE_NEXT) /* Next */
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
void PlayerManager_Init(void)
{
    /* khởi tạo state system */
    gsPlayerContext.mainState = MAIN_STATE_MENU;
    gsPlayerContext.buttonState = BTN_STATE_IDLE;

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
    uint32_t lu32button_evt;
    static TickType_t lu32LastClickTime = 0;

    while (1)
    {
        /* Đợi notification từ button */
        if (xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY) == pdTRUE)
        {
            switch (lu32button_evt)
            {
                case BTN_EVENT_NEXT:
                    /* xử lý next */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON UP\n");
                        #endif
                        /* update cursor */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_PREV);
                        /* Update trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_UP;
                        /* trigger notification */
                        xTaskNotifyGive(oled_taskHandle);
                    }
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON NEXT\n");
                        #endif
                        /* Update trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_NEXT;
                        /* update cursor */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_NEXT);
                        /* update current song */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                    }
                    break;
                case BTN_EVENT_PREV:
                    /* xử lý prev */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON DOWN\n");
                        #endif
                        /* update cursor */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_NEXT);
                        /* Update trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_DOWN;
                    }
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON PREV\n");
                        #endif
                        /* Update trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_PREV;
                        /* update cursor */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_PREV);
                        /* update current song */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                    }
                    break;
                case BTN_EVENT_PLAY:
                {
                    TickType_t now = xTaskGetTickCount();
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON SELECT AND START PLAYING\n");
                        #endif
                        /* Update trạng thái Main */
                        gsPlayerContext.mainState = MAIN_STATE_PLAYING;
                        /* Update trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_PLAY_NEW;
                        /* update current song */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                    }
                    else if (gsPlayerContext.mainState == MAIN_STATE_PLAYING)
                    {
                        /* CHECK DOUBLE CLICK */
                        if ((lu32LastClickTime != 0) && ((now - lu32LastClickTime) < DOUBLE_CLICK_TIME))
                        {
                            #if defined DEVELOPER_CONFIGURATION
                            printf("DOUBLE CLICK -> MENU\n");
                            #endif
                            /* Double click → về MENU */
                            /* Update trạng thái Main */
                            gsPlayerContext.mainState = MAIN_STATE_MENU;
                            /* Update trạng thái button */
                            gsPlayerContext.buttonState = BTN_STATE_IDLE;
                            /* Reset tránh lặp */
                            lu32LastClickTime = 0;
                        }
                        else
                        {
                            /* Single click → play/pause */
                            #if defined DEVELOPER_CONFIGURATION
                            printf("SINGLE CLICK\n");
                            #endif
                            if (gsPlayerContext.buttonState == BTN_STATE_PLAY)
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PAUSE\n");
                                #endif
                                /* Update trạng thái button */
                                gsPlayerContext.buttonState = BTN_STATE_PAUSE;
                            }
                            else
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PLAY\n");
                                #endif
                                /* Update trạng thái button */
                                gsPlayerContext.buttonState = BTN_STATE_PLAY;
                            }
                            /* Update thời gian click cuối cùng */
                            lu32LastClickTime = now;
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