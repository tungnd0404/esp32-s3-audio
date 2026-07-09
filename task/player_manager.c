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

/* Thời gian không thao tác gì ở MENU (sau khi double click từ PLAYING) trước khi tự quay lại PLAYING */
#define MENU_AUTO_RETURN_TIME   pdMS_TO_TICKS(3000)

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */
/* queue khởi tạo */
QueueHandle_t xOledQueue;
QueueHandle_t xMp3Queue;

/* task handler Player Manager*/
TaskHandle_t xPlayerManagerTaskHandle = NULL;

/* state system, khai báo extern trong player_manager.h để oled/menu dùng chung */
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
    /* Giá trị sự kiện nút bấm nhận được từ button ISR (BTN_EVENT_NEXT/PREV/PLAY) */
    uint32_t lu32button_evt;
    /* Lưu thời điểm click PLAY gần nhất để phát hiện double click, static để giữ giá trị qua các vòng lặp */
    static TickType_t lu32LastClickTime = 0;
    /* true khi vừa double click từ PLAYING sang MENU -> đang chờ auto-return sau MENU_AUTO_RETURN_TIME nếu không thao tác gì */
    static bool lbAutoReturnPending = false;
    /* Lưu lại trạng thái play/pause trước khi double click về MENU để auto-return khôi phục đúng trạng thái */
    static PlayerManager_ButtonStateType_e lePrevButtonState = BTN_STATE_PLAY;

    while (1)
    {
        /* Nếu đang chờ auto-return thì chỉ chờ tối đa MENU_AUTO_RETURN_TIME, ngược lại chờ vô thời hạn như bình thường */
        TickType_t lu32WaitTicks;
        if (lbAutoReturnPending)
        {
            lu32WaitTicks = MENU_AUTO_RETURN_TIME;
        }
        else
        {
            lu32WaitTicks = portMAX_DELAY;
        }

        /* Chờ button ISR gửi notification tới, hoặc hết thời gian chờ (chỉ khi lbAutoReturnPending) */
        if (xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, lu32WaitTicks) == pdTRUE)
        {
            switch (lu32button_evt)
            {
                case BTN_EVENT_NEXT:
                    /* Đang ở MENU: nút Next dùng để di chuyển con trỏ xuống dưới danh sách */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON DOWN\n");
                        #endif
                        /* Tính lại vị trí con trỏ mới (đi xuống, quay vòng khi hết danh sách) */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_NEXT);
                        /* Lưu trạng thái button để oled_task biết chiều di chuyển */
                        gsPlayerContext.buttonState = BTN_STATE_DOWN;
                        /* Báo oled_task vẽ lại menu tại vị trí con trỏ mới */
                        xTaskNotify(xOledTaskHandle, OLED_EVENT_MENU_REDRAW, eSetValueWithOverwrite);
                    }
                    /* Đang PLAYING: nút Next dùng để chuyển sang bài kế tiếp */
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                            printf("BUTTON NEXT\n");
                        #endif
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_NEXT;
                        /* Cursor cũng chính là chỉ số bài hát khi đang phát nhạc */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_NEXT);
                        /* Cập nhật bài hát đang phát theo cursor mới */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Báo oled_task: đổi bài -> thoát vòng animation hiện tại, load lại double buffer cho bài mới */
                        xTaskNotify(xOledTaskHandle, OLED_EVENT_SONG_CHANGED, eSetValueWithOverwrite);
                    }
                    break;

                case BTN_EVENT_PREV:
                    /* Đang ở MENU: nút Prev dùng để di chuyển con trỏ lên trên danh sách */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON UP\n");
                        #endif
                        /* Tính lại vị trí con trỏ mới (đi lên, quay vòng khi về đầu danh sách) */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_PREV);
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_UP;
                        /* Báo oled_task vẽ lại menu tại vị trí con trỏ mới */
                        xTaskNotify(xOledTaskHandle, OLED_EVENT_MENU_REDRAW, eSetValueWithOverwrite);
                    }
                    /* Đang PLAYING: nút Prev dùng để quay lại bài trước đó */
                    else
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON PREV\n");
                        #endif
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_PREV;
                        /* Cursor cũng chính là chỉ số bài hát khi đang phát nhạc */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, BTN_STATE_PREV);
                        /* Cập nhật bài hát đang phát theo cursor mới */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Báo oled_task: đổi bài -> thoát vòng animation hiện tại, load lại double buffer cho bài mới */
                        xTaskNotify(xOledTaskHandle, OLED_EVENT_SONG_CHANGED, eSetValueWithOverwrite);
                    }
                    break;

                case BTN_EVENT_PLAY:
                {
                    /* Thời điểm hiện tại, dùng để so sánh phát hiện double click */
                    TickType_t lnow = xTaskGetTickCount();

                    /* Đang ở MENU: nút Play dùng để chọn bài tại vị trí cursor và bắt đầu phát */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEVELOPER_CONFIGURATION
                        printf("BUTTON SELECT AND START PLAYING\n");
                        #endif
                        /* Chuyển sang trạng thái Main là PLAYING */
                        gsPlayerContext.mainState = MAIN_STATE_PLAYING;
                        /* Đánh dấu vừa chọn bài mới để bắt đầu phát từ đầu */
                        gsPlayerContext.buttonState = BTN_STATE_PLAY_NEW;
                        /* Bài đang phát chính là bài tại vị trí cursor */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Chọn bài trực tiếp -> không cần chờ auto-return nữa */
                        lbAutoReturnPending = false;
                        /* Báo oled_task: có bài mới -> load double buffer và bắt đầu vòng animation */
                        xTaskNotify(xOledTaskHandle, OLED_EVENT_SONG_CHANGED, eSetValueWithOverwrite);
                    }
                    /* Đang PLAYING: nút Play dùng để play/pause (bấm 1 lần) hoặc thoát về MENU (bấm 2 lần liên tiếp) */
                    else if (gsPlayerContext.mainState == MAIN_STATE_PLAYING)
                    {
                        /* Double click: khoảng cách giữa 2 lần bấm nhỏ hơn DOUBLE_CLICK_TIME */
                        if ((lu32LastClickTime != 0) && ((lnow - lu32LastClickTime) < DOUBLE_CLICK_TIME))
                        {
                            #if defined DEVELOPER_CONFIGURATION
                            printf("DOUBLE CLICK -> MENU\n");
                            #endif
                            /* Lưu lại trạng thái play/pause hiện tại để lát auto-return khôi phục đúng */
                            lePrevButtonState = gsPlayerContext.buttonState;
                            /* Double click -> thoát phát nhạc, quay lại MENU (nhạc vẫn phát nền, chỉ đổi giao diện) */
                            gsPlayerContext.mainState = MAIN_STATE_MENU;
                            /* Đưa trạng thái button về IDLE để hiển thị menu bình thường */
                            gsPlayerContext.buttonState = BTN_STATE_IDLE;
                            /* Reset mốc thời gian, tránh double click bị tính lặp lại nhiều lần */
                            lu32LastClickTime = 0;
                            /* Bật cờ chờ auto-return: nếu ở MENU quá MENU_AUTO_RETURN_TIME mà không thao tác gì sẽ tự quay lại PLAYING */
                            lbAutoReturnPending = true;
                            /* Báo oled_task: thoát PLAYING -> vẽ lại màn hình MENU */
                            xTaskNotify(xOledTaskHandle, OLED_EVENT_MENU_REDRAW, eSetValueWithOverwrite);
                        }
                        /* Single click: play/pause bài đang phát -> không đổi bài, chỉ báo oled_task tiếp tục/tạm dừng animation */
                        else
                        {
                            #if defined DEVELOPER_CONFIGURATION
                            printf("SINGLE CLICK\n");
                            #endif
                            if (gsPlayerContext.buttonState == BTN_STATE_PLAY)
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PAUSE\n");
                                #endif
                                /* Đang phát -> chuyển sang tạm dừng */
                                gsPlayerContext.buttonState = BTN_STATE_PAUSE;
                                /* Báo oled_task dừng animation lại theo, không load lại bài */
                                xTaskNotify(xOledTaskHandle, OLED_EVENT_PLAY_PAUSE, eSetValueWithOverwrite);
                            }
                            else
                            {
                                #if defined DEVELOPER_CONFIGURATION
                                printf("PLAY\n");
                                #endif
                                /* Đang tạm dừng/mới chọn bài -> chuyển sang phát */
                                gsPlayerContext.buttonState = BTN_STATE_PLAY;
                                /* Báo oled_task tiếp tục animation, không load lại bài */
                                xTaskNotify(xOledTaskHandle, OLED_EVENT_PLAY_PAUSE, eSetValueWithOverwrite);
                            }
                            /* Lưu lại mốc thời gian click này để lần bấm PLAY kế tiếp xét double click */
                            lu32LastClickTime = lnow;
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }
        /* Không nhận được notification nào trong lu32WaitTicks -> chỉ xảy ra khi đang chờ auto-return */
        else if (lbAutoReturnPending)
        {
            #if defined DEVELOPER_CONFIGURATION
            printf("MENU IDLE TIMEOUT -> BACK TO PLAYING\n");
            #endif
            /* Hết 3s không thao tác gì ở MENU -> tự quay lại giao diện PLAYING của bài đang phát */
            gsPlayerContext.mainState = MAIN_STATE_PLAYING;
            /* Khôi phục lại đúng trạng thái play/pause trước khi double click vào MENU */
            gsPlayerContext.buttonState = lePrevButtonState;
            /* Tắt cờ chờ auto-return, quay lại chờ notify vô thời hạn như bình thường */
            lbAutoReturnPending = false;
            /* Báo oled_task quay lại màn hình playing, không cần load lại bài (bài không đổi) */
            xTaskNotify(xOledTaskHandle, OLED_EVENT_PLAY_PAUSE, eSetValueWithOverwrite);
        }
    }
}