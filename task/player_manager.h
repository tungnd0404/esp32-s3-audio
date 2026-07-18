#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Bit riêng cho sự kiện "bài đang phát đã hết" - Pcm_Task tự phát hiện khi đọc hết file .pcm
   (xem Pcm_StreamSong trong pcm_player.c) rồi gửi bit này tới PlayerManager_Task qua CÙNG
   kênh task notification bitmask với nút bấm (BIT3, không đụng BIT0-2 đã dùng cho
   1U<<BTN_EVENT_NEXT/PREV/PLAY, xem button.c) - tận dụng lại đúng cơ chế eSetBits chống mất
   sự kiện sẵn có (vd nếu người dùng bấm Next đúng lúc bài vừa hết, cả 2 sự kiện vẫn được xử
   lý đủ, không cái nào đè mất cái nào). KHÔNG phải BTN_EVENT_* vì đây không phải nút bấm vật
   lý - PlayerManager_Task xử lý bit này bằng đúng logic "chuyển sang bài kế tiếp" (giống
   Next) nhưng KHÔNG phụ thuộc mainState như Next thật (bài phát NỀN hết thì luôn phải chuyển
   bài kế tiếp và tiếp tục phát nền, bất kể người dùng đang xem MENU hay màn hình PLAYING) */
#define PCM_SONG_FINISHED_BIT   (1U << 3)

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* Trạng thái giao diện chính */
typedef enum {
    MAIN_STATE_MENU,
    MAIN_STATE_PLAYING
} PlayerManager_MainStateType_e;

/* Trạng thái nút bấm: ghi lại hành động vừa xảy ra do 1 lần bấm nút gây ra.
   Luôn tự về BTN_STATE_IDLE ngay sau khi hành động được xử lý xong (button bấm rồi nhả,
   không giữ nguyên giá trị này liên tục) -> không dùng field này để biết nhạc đang PLAY hay
   PAUSE, việc đó do PlayerManager_PlaybackStateType_e/playbackState đảm nhiệm. */
typedef enum {
    BTN_STATE_UP,
    BTN_STATE_DOWN,
    BTN_STATE_PLAY_NEW,
    BTN_STATE_PLAY,
    BTN_STATE_PAUSE,
    BTN_STATE_NEXT,
    BTN_STATE_PREV,
    BTN_STATE_BACK_MENU,   /* Double click khi đang PLAYING -> thoát về màn hình MENU */
    BTN_STATE_IDLE
} PlayerManager_ButtonStateType_e;

/* Trạng thái phát nhạc hiện tại, tồn tại liên tục (không tự về idle) cho tới khi
   người dùng bấm PLAY để đổi trạng thái. Oled_PlayAnimation dựa vào field này để biết
   có nên tiếp tục vẽ animation hay không, thay vì dựa vào buttonState (vốn chỉ tồn tại
   trong khoảnh khắc xử lý 1 lần bấm nút). */
typedef enum {
    PLAYBACK_STATE_IDLE,   /* Chưa từng chọn bài nào để phát */
    PLAYBACK_STATE_PLAY,
    PLAYBACK_STATE_PAUSE
} PlayerManager_PlaybackStateType_e;

/* State toàn hệ thống player */
typedef struct
{
    /* mainState/playbackState: volatile vì được đọc liên tục trong vòng lặp while của
       task khác (Oled_PlayAnimation - oled.c, Pcm_StreamSong - pcm_player.c) ngoài cơ chế
       task notification - không có volatile, compiler được phép cache giá trị trong thanh
       ghi qua nhiều vòng lặp nếu không thấy lời gọi hàm nào "chắc chắn" thay đổi nó, khiến
       vòng lặp có thể không bao giờ nhận ra PlayerManager_Task vừa đổi giá trị (vd không
       dừng lại khi Pause) */
    volatile PlayerManager_MainStateType_e mainState;
    PlayerManager_ButtonStateType_e buttonState;
    volatile PlayerManager_PlaybackStateType_e playbackState;
    uint32_t cursor;
    uint32_t currentSong;
    /* Tổng số bài hát tìm thấy trên thẻ nhớ - nguồn sự thật DUY NHẤT (không có biến rời song
       song kiểu gu16SongCount nữa), do Sdcard_Task ghi trực tiếp ngay trong lúc quét thẻ SD
       (xem Sdcard_ScanAndCreateDb trong sdcard.c), các module khác (menu.c, player_manager.c)
       đều đọc qua đây */
    uint32_t totalSong;
} PlayerManager__PlayerContextType_s;

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

extern TaskHandle_t xPlayerManagerTaskHandle;

/* State toàn hệ thống player, dùng chung cho player_manager/oled/menu */
extern PlayerManager__PlayerContextType_s gsPlayerContext;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief PlayerManager_Init
 * Khởi tạo player manager: set state ban đầu (gsPlayerContext.mainState/buttonState/
 * playbackState). Gọi bởi chính PlayerManager_Task lúc khởi động (đầu task, trước vòng lặp
 * chính), không còn gọi từ app_main() - an toàn vì mainState/buttonState chỉ được chính
 * PlayerManager_Task đọc/ghi, còn playbackState chỉ được Oled_Task/Pcm_Task đọc bên trong
 * Oled_PlayAnimation()/Pcm_StreamSong(), cả 2 hàm đó chỉ chạy sau khi nhận notification từ
 * chính PlayerManager_Task (xem PlayerManager_Task trong player_manager.c).
 * @param
 * @return
 */
void PlayerManager_Init(void);

/**
 * @brief PlayerManager_Task
 * @param
 * @return
 */
void PlayerManager_Task(void *arg);

#endif /* PLAYER_MANAGER_H */
