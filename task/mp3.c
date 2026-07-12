/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include <string.h>
#include "mp3.h"
#include "vs1053.h"
#include "sdcard.h"
#include "player_manager.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số request tối đa có thể chờ xử lý trong xMp3CommandQueue cùng lúc */
#define MP3_COMMAND_QUEUE_LENGTH   4U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Mp3_Task */
TaskHandle_t xMp3TaskHandle = NULL;

/* Hàng đợi lệnh dùng chung tới Mp3_Task, tạo trong Mp3_Init() vì Mp3_Task là owner */
QueueHandle_t xMp3CommandQueue = NULL;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* Trạng thái phát nhạc riêng của Mp3_Task, suy ra từ PlayerManager_ButtonStateType_e nhận
   được qua notification. Cùng kiểu thiết kế với Oled_DisplayStateType_e (oled.c) và
   Sdcard_LoadStateType_e (sdcard.c): chỉ dùng nội bộ file này để switch cho tường minh,
   không phải kiểu dữ liệu đi qua kênh notification. */
typedef enum {
    MP3_STATE_SONG_CHANGED,  /* Đổi bài -> đóng file cũ, mở file mới, bắt đầu stream lại từ đầu */
    MP3_STATE_PLAY_PAUSE,    /* Play/Pause đổi trạng thái, không đổi bài -> tiếp tục/tạm dừng stream */
    MP3_STATE_NONE           /* Sự kiện không liên quan (di chuyển cursor, peek MENU...) -> bỏ qua */
} Mp3_PlaybackStateType_e;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* File mp3 của bài đang phát, giữ nguyên (không đóng) qua các lần Pause/Resume để lần
   Resume tiếp tục đúng vị trí đang dừng, thay vì phát lại từ đầu. Chỉ đóng và mở lại khi
   THỰC SỰ đổi bài (xem Mp3_StreamCurrentSong). NULL khi chưa có bài nào đang mở. */
static FILE *gpMp3File = NULL;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_GetPlaybackState
 * Suy ra Mp3_PlaybackStateType_e tương ứng từ buttonState nhận được qua notification.
 * @param buttonState: giá trị PlayerManager_ButtonStateType_e nhận từ PlayerManager_Task
 * @return Mp3_PlaybackStateType_e tương ứng
 */
static Mp3_PlaybackStateType_e Mp3_GetPlaybackState(PlayerManager_ButtonStateType_e buttonState)
{
    switch (buttonState)
    {
        case BTN_STATE_NEXT:
        case BTN_STATE_PREV:
        case BTN_STATE_PLAY_NEW:
            return MP3_STATE_SONG_CHANGED;

        case BTN_STATE_PLAY:
        case BTN_STATE_PAUSE:
            return MP3_STATE_PLAY_PAUSE;

        case BTN_STATE_UP:
        case BTN_STATE_DOWN:
        case BTN_STATE_BACK_MENU:
        case BTN_STATE_IDLE:
        default:
            return MP3_STATE_NONE;
    }
}

/**
 * @brief Mp3_HandleCommand
 * Xử lý 1 request nhận được từ xMp3CommandQueue (xem srm.h - kiến trúc Owner Task): dựa
 * vào cmdId gọi đúng API vs1053_* tương ứng, luôn trả lời qua Srm_Reply() dù cmdId có
 * hợp lệ hay không (bên gửi đang chờ, không trả lời thì Srm_SendCommand() của họ sẽ phải
 * đợi hết timeout mới coi là lỗi, gây chậm không cần thiết).
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @param pRequest: request nhận được từ xMp3CommandQueue
 * @return
 */
static void Mp3_HandleCommand(vs1053_handle_t *pDev, const Srm_Message_s *pRequest)
{
    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        case MP3_CMD_GET_DECODE_TIME:
            /* vs1053_get_decoded_time() trả uint16_t, payload của Srm_Message_s là uint32_t
               thô -> ép kiểu mở rộng, bên nhận (sync_frame.c) sẽ tự ép lại về uint16_t */
            Srm_Reply(pRequest, (uint32_t)vs1053_get_decoded_time(pDev));
            break;

        default:
            /* cmdId lạ (chưa định nghĩa xử lý) -> vẫn trả lời 0 để bên gửi không phải chờ
               hết timeout, tránh trường hợp SRM_CMD_INVALID lọt qua được Srm_SendCommand */
            Srm_Reply(pRequest, 0U);
            break;
    }
}

/**
 * @brief Mp3_ServicePendingCommand
 * Check non-blocking (timeout = 0) xem xMp3CommandQueue có request nào đang chờ không,
 * có thì xử lý ngay. Gọi mỗi vòng lặp trong Mp3_StreamCurrentSong() để các request như
 * MP3_CMD_GET_DECODE_TIME (sync_frame.c gọi liên tục để đồng bộ animation) được trả lời
 * kịp thời trong lúc đang phát nhạc.
 * LƯU Ý: hàm này KHÔNG được gọi khi Mp3_Task đang rảnh/tạm dừng (đang chờ notify ở vòng
 * lặp ngoài Mp3_Task, xTaskNotifyWait dùng portMAX_DELAY) - lúc đó request tới xMp3CommandQueue
 * sẽ không có ai xử lý cho tới khi có bài phát tiếp theo. Bên gửi (Srm_SendCommand) đã có
 * cơ chế timeout + trả về giá trị cũ nên không bị treo, nhưng cần biết giới hạn này.
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @return
 */
static void Mp3_ServicePendingCommand(vs1053_handle_t *pDev)
{
    Srm_Message_s lRequest;

    if (xQueueReceive(xMp3CommandQueue, &lRequest, 0) == pdTRUE)
    {
        Mp3_HandleCommand(pDev, &lRequest);
    }
}

/**
 * @brief Mp3_StreamCurrentSong
 * Stream file mp3 của bài hát ĐANG THỰC SỰ PHÁT (gsPlayerContext.currentSong) ra VS1053,
 * từng khối VS1053_CHUNK_SIZE byte một, cho tới khi Pause/đổi bài/hết file. Cùng khuôn
 * mẫu với Oled_PlayAnimation() (oled.c) và Sdcard_LoadCurrentSong() (sdcard.c): mỗi vòng
 * lặp đều check non-blocking notification để không trễ khi cần xử lý sự kiện khác, kết
 * quả trả về qua tham số ra để vòng lặp ngoài không bị mất giá trị notification vừa nhận.
 *
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @param bIsNewSong: true nếu đây là bài MỚI (Next/Prev/chọn bài từ MENU) -> đóng file cũ
 *        (nếu có) và mở lại file mới, phát từ đầu. false nếu chỉ là Resume sau Pause ->
 *        giữ nguyên gpMp3File đang mở, tiếp tục đọc từ đúng vị trí đang dừng.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi
 *        hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu thoát do lỗi
 *         đọc thông tin bài hát/mở file thất bại, hoặc đã phát hết file (EOF)
 */
static bool Mp3_StreamCurrentSong(vs1053_handle_t *pDev, bool bIsNewSong, uint32_t *pu32NotifyValue)
{
    uint8_t lau8Chunk[VS1053_CHUNK_SIZE];
    size_t lChunkLen;

    if (bIsNewSong == true)
    {
        Sdcard_SongDbType_s lSong;

        /* Đổi bài thật -> đóng file của bài cũ trước khi mở file mới */
        if (gpMp3File != NULL)
        {
            fclose(gpMp3File);
            gpMp3File = NULL;
        }

        /* Không dùng gsPlayerContext.cursor - vì cursor có thể đã di chuyển sang bài khác
           nếu người dùng đang duyệt MENU trong lúc nhạc vẫn phát nền (tính năng auto-return) */
        if (Sdcard_GetSongByIndex((uint16_t)gsPlayerContext.currentSong, &lSong) == false)
        {
            return false;
        }

        gpMp3File = fopen(lSong.songPath, "rb");
        if (gpMp3File == NULL)
        {
            return false;
        }

        /* Reset trạng thái stream nội bộ của VS1053 cho bài mới (end_fill_byte...) */
        vs1053_start_song(pDev);
    }

    /* Phòng thủ: nhận MP3_STATE_PLAY_PAUSE (bIsNewSong=false) khi chưa từng mở bài nào -
       về lý thuyết không xảy ra vì PlayerManager_Task luôn gửi SONG_CHANGED trước, nhưng
       nếu thiếu gpMp3File thì không có gì để đọc, tránh gọi fread() trên con trỏ NULL */
    if (gpMp3File == NULL)
    {
        return false;
    }

    /* Còn stream khi: đang ở màn hình PLAYING và playbackState không phải PAUSE.
       Dùng playbackState (tồn tại liên tục) thay vì buttonState (chỉ tồn tại trong
       khoảnh khắc xử lý 1 lần bấm nút rồi tự về IDLE) - giống hệt Oled_PlayAnimation() */
    while ((gsPlayerContext.mainState == MAIN_STATE_PLAYING) && (gsPlayerContext.playbackState != PLAYBACK_STATE_PAUSE))
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ.
           File KHÔNG bị đóng ở đây (kể cả khi lý do thoát là Pause) - xem comment gpMp3File */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Tranh thủ trả lời các request đang chờ (vd sync_frame.c hỏi decode time) trong
           lúc đang chạy vòng lặp này, xem Mp3_ServicePendingCommand() */
        Mp3_ServicePendingCommand(pDev);

        /* Đọc 1 khối dữ liệu mới rồi gửi cho VS1053. vs1053_send_buffer() tự chờ chân DREQ
           bên trong (có vTaskDelay, không busy-wait) nên vòng lặp này tự nhường CPU đúng
           theo tốc độ VS1053 tiêu thụ dữ liệu, không cần vTaskDelay thủ công như Oled_Task */
        lChunkLen = fread(lau8Chunk, 1, VS1053_CHUNK_SIZE, gpMp3File);
        if (lChunkLen == 0)
        {
            /* Hết file -> đã phát xong bài này, đóng file và dừng chờ sự kiện kế tiếp
               (đổi bài / bấm Play lại từ đầu...). KHÔNG tự động chuyển bài kế tiếp. */
            fclose(gpMp3File);
            gpMp3File = NULL;
            vs1053_stop_song(pDev);
            break;
        }

        vs1053_send_buffer(pDev, lau8Chunk, lChunkLen);
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào.
 * Gọi trước khi tạo Mp3_Task.
 * @param
 * @return
 */
void Mp3_Init(void)
{
    xMp3CommandQueue = xQueueCreate(MP3_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
}

/**
 * @brief Mp3_Task
 * Task owner duy nhất của thiết bị VS1053, stream file mp3 của bài đang phát ra loa.
 * Cùng khuôn thuật toán với Oled_Task (oled.c)/Sdcard_Task (sdcard.c) để các task trong
 * hệ thống đọc thống nhất, dễ theo dõi:
 *
 * Cơ chế nhận sự kiện:
 * - Không tự poll trạng thái theo chu kỳ, mà "ngủ" (xTaskNotifyWait, portMAX_DELAY) chờ
 *   PlayerManager_Task gửi task notification, tiết kiệm CPU khi không có gì thay đổi và
 *   phản ứng ngay lập tức khi có sự kiện.
 * - Giá trị notification nhận được chính là gsPlayerContext.buttonState (kiểu
 *   PlayerManager_ButtonStateType_e) tại thời điểm PlayerManager_Task gửi đi.
 * - Giá trị đó được map qua Mp3_GetPlaybackState() sang Mp3_PlaybackStateType_e (state
 *   riêng của Mp3_Task, chỉ dùng nội bộ file này) để switch bên dưới đọc tường minh.
 *
 * Mp3_Task còn có kênh input thứ 2 mà Oled_Task/Sdcard_Task không có: xMp3CommandQueue
 * (request/response theo kiến trúc Owner Task, xem srm.h) - các module khác (vd
 * sync_frame.c) gửi lệnh đọc/ghi VS1053 vào đây thay vì gọi thẳng API vs1053_*. Kênh này
 * chỉ được xử lý (Mp3_ServicePendingCommand) bên trong Mp3_StreamCurrentSong() - tức CHỈ
 * lúc đang thực sự stream nhạc. Nếu Mp3_Task đang rảnh (chưa phát bài nào) hoặc đang
 * Pause, request sẽ không có ai trả lời cho tới khi phát nhạc trở lại - đây là giới hạn
 * đã biết, chấp nhận được vì Srm_SendCommand() phía bên gửi luôn có timeout + giá trị cũ
 * để dùng tạm, không bị treo.
 *
 * 3 nhánh xử lý theo Mp3_PlaybackStateType_e:
 * - MP3_STATE_SONG_CHANGED: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> Mp3_StreamCurrentSong(..., bIsNewSong=true, ...) đóng file cũ, mở file mới, phát
 *   lại từ đầu.
 * - MP3_STATE_PLAY_PAUSE: bấm Play/Pause, bài không đổi -> Mp3_StreamCurrentSong(...,
 *   bIsNewSong=false, ...), KHÔNG mở lại file -> nếu playbackState đang là PLAY thì tiếp
 *   tục đọc file từ đúng vị trí đang dừng (nhờ gpMp3File không bị đóng lúc Pause), nếu
 *   đang là PAUSE thì vòng lặp bên trong thoát ngay lập tức (không đọc/gửi gì).
 * - MP3_STATE_NONE: sự kiện không liên quan (di chuyển cursor MENU, peek MENU...) -> bỏ qua.
 *
 * Cơ chế lbHasPendingNotify: Mp3_StreamCurrentSong() có thể thoát sớm vì vừa nhận 1
 * notification mới (thay vì thoát vì Pause/hết file) và đã đọc giá trị đó vào
 * lu32NotifyValue. Nếu vòng lặp ngoài gọi xTaskNotifyWait() lần nữa ngay lúc đó thì giá
 * trị vừa nhận sẽ bị mất (thay bằng giá trị chờ tiếp theo). lbHasPendingNotify = true báo
 * cho vòng lặp biết: bỏ qua bước chờ, xử lý ngay lu32NotifyValue đang có sẵn ở vòng lặp
 * kế tiếp.
 *
 * @param pvParameters
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Mp3_Task(void *pvParameters)
{
    /* Device VS1053 - biến local của task này, không cần chia sẻ ra ngoài vì Mp3_Task là
       nơi DUY NHẤT được đụng vào phần cứng (kiến trúc Owner Task) */
    vs1053_handle_t lDevice;
    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32NotifyValue;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32NotifyValue đã có sẵn
       1 giá trị mới cần xử lý ngay (do Mp3_StreamCurrentSong vừa trả về) */
    bool lbHasPendingNotify = false;

    /* Gán chân GPIO thật của board trước khi gọi vs1053_init() - vs1053_init() đọc lại
       các field này để tự cấu hình GPIO cho từng chân */
    memset(&lDevice, 0, sizeof(lDevice));
    lDevice.cs_pin = VS1053_CS_PIN;
    lDevice.dcs_pin = VS1053_DCS_PIN;
    lDevice.dreq_pin = VS1053_DREQ_PIN;
    lDevice.reset_pin = VS1053_RESET_PIN;

    /* vs1053_init() đã tự làm đầy đủ: reset phần cứng, khởi tạo SPI, kiểm tra kết nối,
       cấu hình clock/audio/mode (SM_SDINEW|SM_LINE1) và volume mặc định (50%) - không cần
       gọi thêm vs1053_switch_to_mp3_mode() ngay sau vì nội dung gần như trùng lặp, hàm đó
       chỉ dành cho việc CHUYỂN LẠI chế độ MP3 sau này nếu có nhu cầu (vd sau khi đổi mode
       khác), không cần thiết lúc khởi động lần đầu. */
    if (vs1053_init(&lDevice) != ESP_OK)
    {
        /* Không có VS1053/lỗi kết nối -> không có gì để làm, task nằm im (không return -
           1 FreeRTOS task function không được phép return), tránh gọi tiếp các hàm vs1053_*
           khác trên 1 device chưa chắc đã khởi tạo đúng (spi_handle có thể không hợp lệ) */
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Vòng lặp chính của task, chạy vô hạn cho tới khi thiết bị tắt nguồn */
    while (1)
    {
        /* Check xem có notify nào đang pending không? nếu không thì chờ notify mới,
           chờ vô thời hạn (portMAX_DELAY) vì task không có việc gì khác để làm */
        if (lbHasPendingNotify == false)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32NotifyValue, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Mp3_StreamCurrentSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Map buttonState vừa nhận sang playback state riêng của Mp3_Task rồi rẽ nhánh */
        switch (Mp3_GetPlaybackState((PlayerManager_ButtonStateType_e)lu32NotifyValue))
        {
            case MP3_STATE_SONG_CHANGED:
                lbHasPendingNotify = Mp3_StreamCurrentSong(&lDevice, true, &lu32NotifyValue);
                break;

            case MP3_STATE_PLAY_PAUSE:
                lbHasPendingNotify = Mp3_StreamCurrentSong(&lDevice, false, &lu32NotifyValue);
                break;

            case MP3_STATE_NONE:
            default:
                /* Sự kiện không liên quan tới phát nhạc -> bỏ qua */
                break;
        }
    }
}
