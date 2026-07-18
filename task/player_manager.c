/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "player_manager.h"
#include "feature_config.h"
#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "oled.h"
#include "sdcard.h"
#include "pcm_player.h"
#include "esp_log.h"

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

/* task handler Player Manager*/
TaskHandle_t xPlayerManagerTaskHandle = NULL;

/* state system, khai báo extern trong player_manager.h để oled/menu dùng chung */
PlayerManager__PlayerContextType_s gsPlayerContext;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* Hướng di chuyển cursor, chỉ dùng nội bộ cho PlayerManager_Update_Cursor
   (tách riêng khỏi PlayerManager_ButtonStateType_e vì đây là 2 khái niệm khác nhau:
   1 bên là hành động nút bấm, 1 bên là hướng di chuyển thuần tuý) */
typedef enum {
    CURSOR_DIR_DOWN,
    CURSOR_DIR_UP
} PlayerManager_CursorDirectionType_e;

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
 * @param direction: hướng di chuyển (CURSOR_DIR_DOWN/CURSOR_DIR_UP)
 * @return cursor đã được update
 */
static uint32_t PlayerManager_Update_Cursor(uint32_t cursor, uint32_t totalItem, PlayerManager_CursorDirectionType_e direction)
{
    /* Check tổng số item */
    if (totalItem == 0)
    {
        return 0;
    }
    /* Update cursor position */
    if (direction == CURSOR_DIR_DOWN)
    {
        cursor = (cursor + 1U) % totalItem;
    }
    else /* CURSOR_DIR_UP */
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
    gsPlayerContext.playbackState = PLAYBACK_STATE_IDLE;
}

/**
 * @brief PlayerManager_Task
 * Task điều khiển trung tâm của hệ thống. Chờ task notification (bitmask 1U << BTN_EVENT_xxx,
 * gộp bằng eSetBits - xem button.c) gửi từ button ISR để nhận biết nút Next/Prev/
 * Play vừa được bấm, sau đó cập nhật
 * gsPlayerContext (mainState, buttonState, playbackState, cursor, currentSong) và báo lại
 * cho các task liên quan (qua task notification, truyền thẳng PlayerManager_ButtonStateType_e):
 * - Oled_Task: luôn được báo cho MỌI thay đổi (vẽ lại menu / đổi bài / tiếp tục hay tạm
 *   dừng animation), kể cả những thay đổi không đổi bài (di chuyển cursor, peek MENU).
 * - Sdcard_Task: chỉ báo khi THỰC SỰ đổi bài (Next/Prev lúc PLAYING, chọn bài mới từ MENU)
 *   để nạp lại frame animation cho bài mới vào double buffer.
 * - Pcm_Task: báo khi đổi bài (như Sdcard_Task) VÀ khi Play/Pause đổi trạng thái (để dừng
 *   hoặc tiếp tục stream audio) - không cần báo khi chỉ di chuyển cursor hay peek MENU vì
 *   nhạc vẫn phát nền không đổi gì trong 2 trường hợp đó.
 * Xử lý:
 * - Di chuyển cursor lên/xuống khi đang ở MENU
 * - Chọn bài và bắt đầu phát khi bấm Play ở MENU
 * - Next/Prev sang bài kế/trước khi đang PLAYING
 * - Play/Pause khi bấm 1 lần, thoát về MENU khi bấm Play 2 lần liên tiếp (double click)
 * - Tự động quay lại giao diện PLAYING nếu ở MENU quá MENU_AUTO_RETURN_TIME mà không thao tác gì
 * @return
 */
void PlayerManager_Task(void *arg)
{
    /* Khởi tạo state system TRƯỚC KHI vào vòng lặp chính - gsPlayerContext.mainState/
       buttonState chỉ được chính task này đọc/ghi (không task nào khác đọc trực tiếp từ
       struct, chỉ nhận qua giá trị notification PlayerManager_Task tự gửi), playbackState
       chỉ được Oled_Task/Pcm_Task đọc bên trong Oled_PlayAnimation()/Pcm_StreamSong() - cả 2
       hàm đó chỉ chạy sau khi nhận notification từ chính task này, tức chắc chắn sau dòng
       này - nên gọi ở đây an toàn, không cần gọi từ app_main() nữa */
    PlayerManager_Init();

    /* Giá trị sự kiện nút bấm nhận được từ button ISR - bitmask 1U << BTN_EVENT_xxx (xem
       button.h/button.c), có thể có nhiều hơn 1 bit cùng lúc */
    uint32_t lu32button_evt;
    /* Lưu thời điểm click PLAY gần nhất để phát hiện double click, static để giữ giá trị qua các vòng lặp */
    static TickType_t lu32LastClickTime = 0;
    /* true khi vừa double click từ PLAYING sang MENU -> đang chờ auto-return sau MENU_AUTO_RETURN_TIME nếu không thao tác gì */
    static bool lbAutoReturnPending = false;
    /* Lưu lại trạng thái phát nhạc trước khi double click về MENU để auto-return khôi phục đúng trạng thái */
    static PlayerManager_PlaybackStateType_e lePrevPlaybackState = PLAYBACK_STATE_PLAY;

    /* Vòng lặp chính của task, chạy vô hạn cho tới khi thiết bị tắt nguồn */
    while (1)
    {
        /* Nếu đang chờ auto-return thì chỉ chờ tối đa MENU_AUTO_RETURN_TIME, ngược lại chờ vô thời hạn như bình thường */
        TickType_t lu32WaitTicks;
        if (lbAutoReturnPending == true)
        {
            lu32WaitTicks = MENU_AUTO_RETURN_TIME;
        }
        else
        {
            lu32WaitTicks = portMAX_DELAY;
        }

        /* Chờ button ISR gửi notification tới, hoặc hết thời gian chờ (chỉ khi lbAutoReturnPending == true) */
        if (xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, lu32WaitTicks) == pdTRUE)
        {
            /* lu32button_evt giờ là BITMASK các nút vừa được bấm (mỗi bit = 1U << BTN_EVENT_xxx,
               xem button.c, gộp bằng eSetBits) - CÓ THỂ có nhiều hơn 1 bit cùng lúc
               nếu 2 nút khác nhau được bấm gần như đồng thời trước khi task kịp thức dậy xử lý
               (xem giải thích chi tiết trong button.c). Xử lý TỪNG bit độc lập theo thứ tự cố
               định Next -> Prev -> Play thay vì switch trên 1 giá trị duy nhất như trước - đảm
               bảo không sự kiện nào bị bỏ sót dù bao nhiêu bit cùng về 1 lượt (khác hành vi ghi
               đè bằng eSetValueWithOverwrite trước đây, có thể làm mất hẳn 1 lần bấm). Thứ tự
               xử lý khi CẢ 2 bit khác nhau cùng về 1 lượt (hiếm, chỉ xảy ra khi 2 nút vật lý bị
               bấm trong cùng vài ms) không đảm bảo phản ánh đúng thứ tự thời gian thực đã bấm -
               đánh đổi chấp nhận được, quan trọng là không bit nào bị mất hẳn. */
            if ((lu32button_evt & (1U << BTN_EVENT_NEXT)) != 0U)
            {
                    /* Đang ở MENU: nút Next dùng để di chuyển con trỏ xuống dưới danh sách */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEBUG_PRINTF_ENABLED
                            printf("BUTTON DOWN\n");
                        #endif
                        /* Tính lại vị trí con trỏ mới (đi xuống, quay vòng khi hết danh sách) */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, CURSOR_DIR_DOWN);
                        /* Lưu trạng thái button để oled_task biết chiều di chuyển */
                        gsPlayerContext.buttonState = BTN_STATE_DOWN;
                        /* Báo oled_task vẽ lại menu tại vị trí con trỏ mới -> truyền thẳng buttonState,
                           không cần enum sự kiện riêng cho OLED vì nội dung y hệt nhau */
                        xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Đã xử lý xong hành động -> đưa buttonState về IDLE (button bấm rồi nhả, không giữ trạng thái tạm này) */
                        gsPlayerContext.buttonState = BTN_STATE_IDLE;
                    }
                    /* Đang PLAYING: nút Next dùng để chuyển sang bài kế tiếp */
                    else
                    {
                        #if defined DEBUG_PRINTF_ENABLED
                            printf("BUTTON NEXT\n");
                        #endif
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_NEXT;
                        /* Cursor cũng chính là chỉ số bài hát khi đang phát nhạc */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, CURSOR_DIR_DOWN);
                        /* Cập nhật bài hát đang phát theo cursor mới */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Báo oled_task: đổi bài -> thoát vòng animation hiện tại, load lại double buffer cho bài mới */
                        xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo sd_task: đổi bài -> nạp lại frame animation cho bài mới vào double buffer */
                        xTaskNotify(xSdTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo mp3_task: đổi bài -> dừng stream bài cũ, phát bài mới */
                        xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                        gsPlayerContext.buttonState = BTN_STATE_IDLE;
                    }
            }

            if ((lu32button_evt & (1U << BTN_EVENT_PREV)) != 0U)
            {
                    /* Đang ở MENU: nút Prev dùng để di chuyển con trỏ lên trên danh sách */
                    if (gsPlayerContext.mainState == MAIN_STATE_MENU)
                    {
                        #if defined DEBUG_PRINTF_ENABLED
                        printf("BUTTON UP\n");
                        #endif
                        /* Tính lại vị trí con trỏ mới (đi lên, quay vòng khi về đầu danh sách) */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, CURSOR_DIR_UP);
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_UP;
                        /* Báo oled_task vẽ lại menu tại vị trí con trỏ mới */
                        xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                        gsPlayerContext.buttonState = BTN_STATE_IDLE;
                    }
                    /* Đang PLAYING: nút Prev dùng để quay lại bài trước đó */
                    else
                    {
                        #if defined DEBUG_PRINTF_ENABLED
                        printf("BUTTON PREV\n");
                        #endif
                        /* Lưu trạng thái button */
                        gsPlayerContext.buttonState = BTN_STATE_PREV;
                        /* Cursor cũng chính là chỉ số bài hát khi đang phát nhạc */
                        gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, CURSOR_DIR_UP);
                        /* Cập nhật bài hát đang phát theo cursor mới */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Báo oled_task: đổi bài -> thoát vòng animation hiện tại, load lại double buffer cho bài mới */
                        xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo sd_task: đổi bài -> nạp lại frame animation cho bài mới vào double buffer */
                        xTaskNotify(xSdTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo mp3_task: đổi bài -> dừng stream bài cũ, phát bài mới */
                        xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                        gsPlayerContext.buttonState = BTN_STATE_IDLE;
                    }
            }

            if ((lu32button_evt & (1U << BTN_EVENT_PLAY)) != 0U)
            {
                    /* Thời điểm hiện tại, dùng để so sánh phát hiện double click */
                    TickType_t lnow = xTaskGetTickCount();

                    /* Đang ở MENU: nút Play dùng để chọn bài tại vị trí cursor và bắt đầu phát -
                       CHỈ khi có ít nhất 1 bài hợp lệ (gsPlayerContext.totalSong != 0). Không
                       guard trước đây khiến hệ thống vẫn chuyển mainState sang PLAYING với
                       currentSong=0 dù không tồn tại bài hát nào (thẻ trống, thẻ lỗi, hoặc
                       không bài nào có đủ cặp .mp3/.bin - xem Sdcard_ScanAndCreateDb trong
                       sdcard.c), kẹt người dùng ở màn hình "đang phát" trống rỗng không lối ra
                       rõ ràng (phải biết double-click - thao tác không hiển nhiên - mới thoát
                       được về MENU). totalSong == 0 thì bỏ qua thao tác Play hoàn toàn, không
                       làm gì (rơi qua nhánh else if PLAYING cũng không khớp vì mainState vẫn
                       là MENU) */
                    if ((gsPlayerContext.mainState == MAIN_STATE_MENU) && (gsPlayerContext.totalSong != 0U))
                    {
                        #if defined DEBUG_PRINTF_ENABLED
                        printf("BUTTON SELECT AND START PLAYING\n");
                        #endif
                        /* Chuyển sang trạng thái Main là PLAYING */
                        gsPlayerContext.mainState = MAIN_STATE_PLAYING;
                        /* Đánh dấu vừa chọn bài mới để bắt đầu phát từ đầu */
                        gsPlayerContext.buttonState = BTN_STATE_PLAY_NEW;
                        /* Bài mới chọn -> tự động phát ngay (autoplay) */
                        gsPlayerContext.playbackState = PLAYBACK_STATE_PLAY;
                        /* Bài đang phát chính là bài tại vị trí cursor */
                        gsPlayerContext.currentSong = gsPlayerContext.cursor;
                        /* Bắt đầu phiên phát mới -> reset mốc double click, tránh mang mốc thời gian
                           của phiên phát trước đó sang gây double-click giả ở lần bấm PLAY đầu tiên */
                        lu32LastClickTime = 0;
                        /* Chọn bài trực tiếp -> không cần chờ auto-return nữa */
                        lbAutoReturnPending = false;
                        /* Báo oled_task: có bài mới -> load double buffer và bắt đầu vòng animation */
                        xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo sd_task: có bài mới -> nạp frame animation của bài này vào double buffer */
                        xTaskNotify(xSdTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Báo mp3_task: có bài mới -> mở file mp3 và bắt đầu stream */
                        xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                        /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                        gsPlayerContext.buttonState = BTN_STATE_IDLE;
                    }
                    /* Đang PLAYING: nút Play dùng để play/pause (bấm 1 lần) hoặc thoát về MENU (bấm 2 lần liên tiếp) */
                    else if (gsPlayerContext.mainState == MAIN_STATE_PLAYING)
                    {
                        /* Double click: khoảng cách giữa 2 lần bấm nhỏ hơn DOUBLE_CLICK_TIME */
                        if ((lu32LastClickTime != 0) && ((lnow - lu32LastClickTime) < DOUBLE_CLICK_TIME))
                        {
                            #if defined DEBUG_PRINTF_ENABLED
                            printf("DOUBLE CLICK -> MENU\n");
                            #endif
                            /* Lưu lại trạng thái phát nhạc hiện tại để lát auto-return khôi phục đúng
                               (buttonState không dùng được cho việc này vì nó tự về IDLE ngay sau khi xử lý) */
                            lePrevPlaybackState = gsPlayerContext.playbackState;
                            /* Double click -> thoát phát nhạc, quay lại MENU (nhạc vẫn phát nền, chỉ đổi giao diện) */
                            gsPlayerContext.mainState = MAIN_STATE_MENU;
                            /* Ghi nhận hành động: double click thoát về MENU (tường minh hơn BTN_STATE_IDLE) */
                            gsPlayerContext.buttonState = BTN_STATE_BACK_MENU;
                            /* Reset mốc thời gian, tránh double click bị tính lặp lại nhiều lần */
                            lu32LastClickTime = 0;
                            /* Bật cờ chờ auto-return: nếu ở MENU quá MENU_AUTO_RETURN_TIME mà không thao tác gì sẽ tự quay lại PLAYING */
                            lbAutoReturnPending = true;
                            /* Báo oled_task: thoát PLAYING -> vẽ lại màn hình MENU */
                            xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                            /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                            gsPlayerContext.buttonState = BTN_STATE_IDLE;
                        }
                        /* Single click: play/pause bài đang phát -> không đổi bài, chỉ báo oled_task tiếp tục/tạm dừng animation */
                        else
                        {
                            #if defined DEBUG_PRINTF_ENABLED
                            printf("SINGLE CLICK\n");
                            #endif
                            if (gsPlayerContext.playbackState == PLAYBACK_STATE_PLAY)
                            {
                                #if defined DEBUG_PRINTF_ENABLED
                                printf("PAUSE\n");
                                #endif
                                /* Ghi nhận hành động vừa bấm là PAUSE (chỉ mang tính thời điểm) */
                                gsPlayerContext.buttonState = BTN_STATE_PAUSE;
                                /* Đang phát -> chuyển sang tạm dừng, giá trị này tồn tại liên tục cho tới lần PLAY kế tiếp */
                                gsPlayerContext.playbackState = PLAYBACK_STATE_PAUSE;
                                /* Báo oled_task dừng animation lại theo, không load lại bài */
                                xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                                /* Báo mp3_task tạm dừng stream (không đổi bài, không cần báo sd_task) */
                                xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                            }
                            else
                            {
                                #if defined DEBUG_PRINTF_ENABLED
                                printf("PLAY\n");
                                #endif
                                /* Ghi nhận hành động vừa bấm là PLAY (chỉ mang tính thời điểm) */
                                gsPlayerContext.buttonState = BTN_STATE_PLAY;
                                /* Đang tạm dừng -> chuyển sang phát */
                                gsPlayerContext.playbackState = PLAYBACK_STATE_PLAY;
                                /* Báo oled_task tiếp tục animation, không load lại bài */
                                xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                                /* Báo mp3_task tiếp tục stream (không đổi bài, không cần báo sd_task) */
                                xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                            }
                            /* Đã xử lý xong hành động -> đưa buttonState về IDLE */
                            gsPlayerContext.buttonState = BTN_STATE_IDLE;
                            /* Lưu lại mốc thời gian click này để lần bấm PLAY kế tiếp xét double click */
                            lu32LastClickTime = lnow;
                        }
                    }
            }

            /* Bài đang phát NỀN đã hết (Pcm_Task tự phát hiện, xem Pcm_StreamSong trong
               pcm_player.c) -> tự động chuyển sang bài kế tiếp và tiếp tục phát, dùng lại
               đúng buttonState/thông báo BTN_STATE_NEXT cho Oled_Task/Sdcard_Task/Pcm_Task
               giống hệt nút Next thật - NHƯNG KHÔNG phụ thuộc mainState như Next thật (Next
               thật lúc ở MENU chỉ di chuyển con trỏ, không đổi bài): dù người dùng đang xem
               MENU hay màn hình PLAYING, bài phát nền hết vẫn phải tự chuyển bài kế tiếp và
               tiếp tục phát nền. totalSong != 0 chỉ để phòng thủ (không thể thực sự bằng 0 ở
               đây vì phải có bài đang phát mới sinh ra được sự kiện này) */
            if (((lu32button_evt & PCM_SONG_FINISHED_BIT) != 0U) && (gsPlayerContext.totalSong != 0U))
            {
                #if defined DEBUG_PRINTF_ENABLED
                printf("SONG FINISHED -> AUTO NEXT\n");
                #endif
                gsPlayerContext.buttonState = BTN_STATE_NEXT;
                gsPlayerContext.cursor = PlayerManager_Update_Cursor(gsPlayerContext.cursor, gsPlayerContext.totalSong, CURSOR_DIR_DOWN);
                gsPlayerContext.currentSong = gsPlayerContext.cursor;
                /* Vẫn đang phát nhạc - chỉ đổi bài, không phải Pause */
                gsPlayerContext.playbackState = PLAYBACK_STATE_PLAY;
                xTaskNotify(xOledTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                xTaskNotify(xSdTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                xTaskNotify(xPcmTaskHandle, gsPlayerContext.buttonState, eSetValueWithOverwrite);
                gsPlayerContext.buttonState = BTN_STATE_IDLE;
            }
        }
        /* Không nhận được notification nào trong lu32WaitTicks -> chỉ xảy ra khi đang chờ auto-return */
        else if (lbAutoReturnPending == true)
        {
            #if defined DEBUG_PRINTF_ENABLED
            printf("MENU IDLE TIMEOUT -> BACK TO PLAYING\n");
            #endif
            /* Hết 3s không thao tác gì ở MENU -> tự quay lại giao diện PLAYING của bài đang phát */
            gsPlayerContext.mainState = MAIN_STATE_PLAYING;
            /* Khôi phục lại đúng trạng thái phát nhạc trước khi double click vào MENU */
            gsPlayerContext.playbackState = lePrevPlaybackState;
            /* Tắt cờ chờ auto-return, quay lại chờ notify vô thời hạn như bình thường */
            lbAutoReturnPending = false;
            /* Báo oled_task quay lại màn hình playing, không cần load lại bài (bài không đổi).
               Không dùng gsPlayerContext.buttonState ở đây vì nó đang là BTN_STATE_IDLE (đã bị
               reset từ lần xử lý trước) -> tự suy ra BTN_STATE_PLAY/PAUSE tương ứng playbackState
               vừa khôi phục để Oled_Task rơi đúng vào nhóm "play/pause" thay vì nhóm "vẽ menu" */
            if (lePrevPlaybackState == PLAYBACK_STATE_PLAY)
            {
                xTaskNotify(xOledTaskHandle, BTN_STATE_PLAY, eSetValueWithOverwrite);
            }
            else
            {
                xTaskNotify(xOledTaskHandle, BTN_STATE_PAUSE, eSetValueWithOverwrite);
            }
        }
    }
}