/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "mp3.h"
#include "vs1053.h"
#include "ring_buffer.h"
#include "player_manager.h"
#include "esp_log.h"
#include "esp_timer.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số request tối đa có thể chờ xử lý trong xMp3CommandQueue cùng lúc */
#define MP3_COMMAND_QUEUE_LENGTH   4U

/* Thời gian tối đa chờ RingBuffer_Read() mỗi vòng lặp trong Mp3_StreamSong() khi
   xMp3RingBuffer đang tạm rỗng. Không dùng portMAX_DELAY - Sdcard_Task nạp lại theo chu kỳ
   ~50ms (SDCARD_LOAD_WAIT_MS trong sdcard.c), chờ dài hơn 1 chu kỳ đó là đủ mà không giữ
   task quá lâu, còn kịp check non-blocking notification + xMp3CommandQueue ở vòng sau */
#define MP3_RING_BUFFER_READ_WAIT_MS   50U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Mp3_Task */
TaskHandle_t xMp3TaskHandle = NULL;

/* Hàng đợi lệnh dùng chung tới Mp3_Task, tạo trong Mp3_Init() vì Mp3_Task là owner */
QueueHandle_t xMp3CommandQueue = NULL;

/* Ring buffer dữ liệu mp3 thô, tạo trong Mp3_Init(). Sdcard_Task ghi, Mp3_Task đọc - xem
   giải thích đầy đủ ở khai báo extern trong mp3.h */
RingbufHandle_t xMp3RingBuffer = NULL;

/* true = Sdcard_Task đã đọc hết bài hiện tại, không còn gì để nạp thêm vào xMp3RingBuffer -
   xem giải thích đầy đủ ở khai báo extern trong mp3.h. Mặc định true (chưa có bài nào mở) */
volatile bool gbMp3StreamEof = true;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Tag dùng cho ESP_LOGx trong module này */
static const char *TAG = "MP3";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_HandleCommand
 * Xử lý 1 request nhận được từ xMp3CommandQueue (xem srm.h - kiến trúc Owner Task): dựa
 * vào cmdId gọi đúng API vs1053_* tương ứng, luôn trả lời qua Srm_Reply() dù cmdId có
 * hợp lệ hay không (bên gửi đang chờ trong Srm_Mp3GetDecodeTime(), không trả lời thì họ sẽ
 * phải đợi hết timeout mới coi là lỗi, gây chậm không cần thiết).
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @param pRequest: request nhận được từ xMp3CommandQueue
 * @return
 */
static void Mp3_HandleCommand(vs1053_handle_t *pDev, const Srm_Message_s *pRequest)
{
    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        case MP3_CMD_GET_DECODE_TIME:
            /* vs1053_get_decoded_time() trả uint16_t, ghi thẳng (mở rộng thành uint32_t vì
               pData cần kiểu cố định) vào pRequest->pData - biến đích của Srm_Mp3GetDecodeTime()
               truyền vào (xem srm.c), payload chỉ còn mang kết quả E_OK/E_NOT_OK */
            *(uint32_t *)pRequest->pData = (uint32_t)vs1053_get_decoded_time(pDev);
            Srm_Reply(pRequest, (uint32_t)E_OK);
            break;

        default:
            /* cmdId lạ (chưa định nghĩa xử lý) -> vẫn trả lời E_NOT_OK để bên gửi không
               phải chờ hết timeout, tránh trường hợp SRM_CMD_INVALID lọt qua được lúc gửi */
            Srm_Reply(pRequest, (uint32_t)E_NOT_OK);
            break;
    }
}

/**
 * @brief Mp3_ServicePendingCommand
 * Check non-blocking (timeout = 0) xem xMp3CommandQueue có request nào đang chờ không,
 * có thì xử lý ngay. Gọi mỗi vòng lặp trong Mp3_StreamSong() để các request như
 * MP3_CMD_GET_DECODE_TIME (sync_frame.c gọi liên tục để đồng bộ animation) được trả lời
 * kịp thời trong lúc đang phát nhạc.
 * LƯU Ý: hàm này KHÔNG được gọi khi Mp3_Task đang rảnh/tạm dừng (đang chờ notify ở vòng
 * lặp ngoài Mp3_Task, xTaskNotifyWait dùng portMAX_DELAY) - lúc đó request tới xMp3CommandQueue
 * sẽ không có ai xử lý cho tới khi có bài phát tiếp theo. Bên gửi (Srm_Mp3GetDecodeTime())
 * đã có cơ chế timeout + trả về giá trị cũ nên không bị treo, nhưng cần biết giới hạn này.
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
 * @brief Mp3_StreamSong
 * Rút dữ liệu mp3 thô từ xMp3RingBuffer, gửi cho VS1053 từng khối tối đa VS1053_CHUNK_SIZE
 * byte một, cho tới khi Pause/đổi bài/hết bài. KHÔNG tự đọc thẻ SD (Mp3_Task không còn là
 * nơi mở/đọc file mp3 - vi phạm kiến trúc Owner Task vì Sdcard_Task mới là owner duy nhất
 * của thẻ SD, xem srm.h) - Sdcard_Task tự mở file mp3 và nạp liên tục vào xMp3RingBuffer
 * ngay khi nhận CÙNG notification đổi bài (xem Sdcard_LoadSong trong sdcard.c), Mp3_Task
 * ở đây chỉ việc rút ra và phát. Chỉ nên được gọi khi playbackState đang là
 * PLAYBACK_STATE_PLAY - PAUSE có hàm riêng Mp3_HandlePause(), không đi qua đây nữa. Có check
 * phòng thủ ngay đầu hàm: nếu lúc gọi playbackState không phải PLAY (gọi sai chỗ) thì thoát
 * ngay, không đọc/gửi byte nào - giống hệt Oled_PlayAnimation() (oled.c). Cùng khuôn mẫu với
 * Oled_PlayAnimation() và Sdcard_LoadSong() (sdcard.c): mỗi vòng lặp đều check
 * non-blocking notification để không trễ khi cần xử lý sự kiện khác, kết quả trả về qua
 * tham số ra để vòng lặp ngoài không bị mất giá trị notification vừa nhận.
 *
 * LƯU Ý: KHÔNG check gsPlayerContext.mainState ở đây (khác với code cũ) - double click về
 * MENU lúc đang PLAYING chỉ báo notify cho Oled_Task (xem player_manager.c), KHÔNG báo
 * Mp3_Task, đúng ý "nhạc vẫn phát nền" khi thoát PLAYING về MENU. Nếu check thêm mainState,
 * vòng lặp bên dưới sẽ tự thoát ngay khi mainState đổi (dù không hề có notify) và Mp3_Task
 * rơi vào chờ vô hạn, dừng phát nhạc nền sai với thiết kế.
 *
 * Bài MỚI (Next/Prev/chọn bài từ MENU) cần reset trạng thái decode nội bộ của VS1053
 * (vs1053_start_song) TRƯỚC KHI gọi hàm này - việc đó do bên gọi tự làm ở switch case tương
 * ứng trong Mp3_Task (giống hệt cách Oled_Task tự gọi SyncFrame_Init() trước
 * Oled_PlayAnimation() thay vì truyền cờ "có phải bài mới không" vào), không phải việc của
 * hàm này.
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi
 *        hàm trả về true
 * @param pbHwFailure: [out] LUÔN được gán (bất kể hàm trả về gì) - true nếu thoát vì
 *        vs1053_send_buffer() báo lỗi giao tiếp (VS1053 không phản hồi DREQ trong
 *        VS1053_DREQ_TIMEOUT_MS, xem vs1053.c) - tức chip coi như đã treo/mất kết nối giữa
 *        chừng, KHÁC với các lý do thoát bình thường (Pause/đổi bài/hết bài tự nhiên). Bên gọi
 *        (Mp3_Task) kiểm tra cờ này để chuyển sang trạng thái halt an toàn thay vì tiếp tục cố
 *        gắng stream vào 1 chip không còn phản hồi.
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu gọi sai lúc
 *         playbackState không phải PLAY, xMp3RingBuffer chưa được khởi tạo, đã phát hết bài
 *         (EOF), hoặc phát hiện lỗi giao tiếp VS1053 (xem *pbHwFailure)
 */
static bool Mp3_StreamSong(vs1053_handle_t *pDev, uint32_t *pu32NotifyValue, bool *pbHwFailure)
{
    *pbHwFailure = false;

    /* Phòng thủ: hàm này chỉ dành cho lúc đang PLAY, không phải nơi xử lý PAUSE */
    if (gsPlayerContext.playbackState != PLAYBACK_STATE_PLAY)
    {
        return false;
    }

    /* Phòng thủ: module chưa init (Mp3_Init() chưa từng gọi) -> không có gì để đọc */
    if (xMp3RingBuffer == NULL)
    {
        return false;
    }

    /* [DEBUG TẠM] in decodeTime/số chunk gửi mỗi ~1s để kiểm tra VS1053 có thực sự giải mã
       tiến triển không (nếu decodeTime đứng yên trong khi chunksPerSec > 0, nghĩa là dữ liệu
       vẫn được gửi đều nhưng chip không giải mã được - xem thêm log [DEBUG] DREQ trong
       vs1053.c). XOÁ khối debug này (3 biến local + đoạn log cuối vòng lặp) sau khi xác định
       xong nguyên nhân. */
    uint32_t lu32DbgLastLogMs = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t lu32DbgChunkCount = 0U;

    /* Vòng lặp stream, chạy tới khi có notification mới cần xử lý (Pause/đổi bài/về menu...)
       hoặc hết bài (EOF) - giống hệt Oled_PlayAnimation() */
    while (1)
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Tranh thủ trả lời các request đang chờ (vd sync_frame.c hỏi decode time) trong
           lúc đang chạy vòng lặp này, xem Mp3_ServicePendingCommand() */
        Mp3_ServicePendingCommand(pDev);

        /* Rút tối đa VS1053_CHUNK_SIZE byte hiện có trong ring buffer. Nếu ring buffer đang
           tạm rỗng, RingBuffer_Read() tự chờ tối đa MP3_RING_BUFFER_READ_WAIT_MS - khoảng
           chờ này chính là "nhịp nghỉ" của vòng lặp (thay cho vTaskDelay thủ công), giống
           cách vs1053_send_buffer() tự chờ DREQ khi có dữ liệu để gửi */
        uint8_t *lpChunk = NULL;
        size_t lChunkLen = RingBuffer_Read(xMp3RingBuffer, (void **)&lpChunk, VS1053_CHUNK_SIZE,
                                            pdMS_TO_TICKS(MP3_RING_BUFFER_READ_WAIT_MS));
        if (lChunkLen == 0)
        {
            if (gbMp3StreamEof == true)
            {
                /* Sdcard_Task đã đọc hết file mp3 VÀ ring buffer cũng đã rút cạn -> hết bài
                   thật sự, dừng chờ sự kiện kế tiếp (đổi bài / bấm Play lại từ đầu...).
                   KHÔNG tự động chuyển bài kế tiếp. */
                vs1053_stop_song(pDev);
                break;
            }

            /* Ring buffer tạm thời rỗng, Sdcard_Task chưa kịp nạp thêm -> thử lại vòng sau,
               không phải hết bài */
            continue;
        }

        if (vs1053_send_buffer(pDev, lpChunk, lChunkLen) != ESP_OK)
        {
            /* VS1053 không phản hồi DREQ trong thời gian cho phép - chip coi như đã treo/mất
               kết nối giữa chừng (DREQ vốn luôn lên cao trong vài ms ở điều kiện bình thường,
               xem VS1053_DREQ_TIMEOUT_MS trong vs1053.c), không phải lỗi tạm thời của 1 lần
               gửi đơn lẻ. Vẫn phải trả lại item cho ring buffer trước khi thoát (BẮT BUỘC theo
               đúng hợp đồng của RingBuffer_Read(), xem ring_buffer.h - không liên quan gì tới
               việc gửi có thành công hay không, chỉ là giải phóng lại chỗ đã đọc) */
            RingBuffer_ReturnItem(xMp3RingBuffer, lpChunk);
            ESP_LOGE(TAG, "VS1053 communication lost while streaming");
            *pbHwFailure = true;
            return false;
        }

        /* [DEBUG TẠM] in vài byte đầu của chunk NGAY TRƯỚC KHI trả lại cho ring buffer (sau
           khi trả, con trỏ lpChunk có thể bị ghi đè bởi lần đọc kế tiếp) - để kiểm tra dữ
           liệu thật sự gửi cho VS1053 có giống MP3 hợp lệ không (frame header MP3 thường bắt
           đầu bằng 0xFF 0xFB/0xFA/0xF3/0xF2...). Chỉ in mỗi ~1s, dùng chung mốc thời gian với
           log chunks/s/decodeTime bên dưới. XOÁ khối debug này sau khi xác định xong nguyên
           nhân. */
        lu32DbgChunkCount++;
        uint32_t lu32DbgNowMs = (uint32_t)(esp_timer_get_time() / 1000);
        if ((lu32DbgNowMs - lu32DbgLastLogMs) >= 1000U)
        {
            size_t lu32DbgDumpLen = (lChunkLen < 8U) ? lChunkLen : 8U;
            ESP_LOGW(TAG, "[DEBUG] chunks/s=%u decodeTime=%u first_bytes=%02X %02X %02X %02X %02X %02X %02X %02X (len=%u)",
                     (unsigned)lu32DbgChunkCount, (unsigned)vs1053_get_decoded_time(pDev),
                     lu32DbgDumpLen > 0 ? lpChunk[0] : 0, lu32DbgDumpLen > 1 ? lpChunk[1] : 0,
                     lu32DbgDumpLen > 2 ? lpChunk[2] : 0, lu32DbgDumpLen > 3 ? lpChunk[3] : 0,
                     lu32DbgDumpLen > 4 ? lpChunk[4] : 0, lu32DbgDumpLen > 5 ? lpChunk[5] : 0,
                     lu32DbgDumpLen > 6 ? lpChunk[6] : 0, lu32DbgDumpLen > 7 ? lpChunk[7] : 0,
                     (unsigned)lChunkLen);
            lu32DbgChunkCount = 0U;
            lu32DbgLastLogMs = lu32DbgNowMs;
        }

        RingBuffer_ReturnItem(xMp3RingBuffer, lpChunk);
    }

    return false;
}

/**
 * @brief Mp3_HandlePause
 * Xử lý lúc playbackState chuyển sang PLAYBACK_STATE_PAUSE: dừng bơm dữ liệu cho VS1053.
 * Không cần lệnh phần cứng nào thêm để "tạm dừng" - VS1053 tự hết âm thanh ngay khi không
 * còn được gọi vs1053_send_buffer() nữa (hết dữ liệu trong FIFO nội bộ của chip), vị trí đọc
 * xMp3RingBuffer vẫn giữ nguyên nên lần Resume sau (Mp3_StreamSong()) phát tiếp
 * đúng chỗ đang dừng. Cùng khuôn mẫu với Oled_DrawPauseIcon() (oled.c) - PAUSE có
 * hàm riêng, không đi qua Mp3_StreamSong() nữa.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp (hiện luôn false vì hàm
 *         không tự chờ notification nào - chỉ đơn thuần dừng lại)
 */
static bool Mp3_HandlePause(uint32_t *pu32NotifyValue)
{
    (void)pu32NotifyValue;
    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào, và
 * xMp3RingBuffer để Sdcard_Task nạp dữ liệu mp3 thô vào cho Mp3_Task rút ra phát (xem mp3.h).
 * Gọi bởi chính Mp3_Task lúc khởi động (đầu Mp3_Task, TRƯỚC vs1053_init() - xem Mp3_Task),
 * không còn ở app_main. Chỉ tạo 2 object trong RAM (không đụng phần cứng) nên chạy gần như
 * tức thời - Sdcard_Task (task khác, có thể chạy song song trên core khác) chỉ ghi vào
 * xMp3RingBuffer lúc thực sự nạp bài (Sdcard_LoadSong, xem sdcard.c), và việc đó luôn cần
 * người dùng bấm nút trước (qua PlayerManager_Task) - độ trễ phản xạ người dùng (tối thiểu
 * hàng chục ms) dư sức lớn hơn thời gian 2 dòng lệnh dưới đây chạy xong, nên không cần cơ chế
 * poll/retry như Srm_OledNotifyBootStatus() (trường hợp đó 2 task tự động đua nhau ngay lúc
 * boot, không có độ trễ người dùng làm đệm).
 * @param
 * @return
 */
void Mp3_Init(void)
{
    xMp3CommandQueue = xQueueCreate(MP3_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
    xMp3RingBuffer = RingBuffer_Init(MP3_RING_BUFFER_SIZE);
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
 * - Switch bên dưới xử lý trực tiếp trên buttonState, không qua enum trung gian.
 *
 * Mp3_Task còn có kênh input thứ 2 mà Oled_Task/Sdcard_Task không có: xMp3CommandQueue
 * (request/response theo kiến trúc Owner Task, xem srm.h) - các module khác (vd
 * sync_frame.c) gửi lệnh đọc/ghi VS1053 vào đây thay vì gọi thẳng API vs1053_*. Kênh này
 * chỉ được xử lý (Mp3_ServicePendingCommand) bên trong Mp3_StreamSong() - tức CHỈ
 * lúc đang thực sự stream nhạc. Nếu Mp3_Task đang rảnh (chưa phát bài nào) hoặc đang
 * Pause, request sẽ không có ai trả lời cho tới khi phát nhạc trở lại - đây là giới hạn
 * đã biết, chấp nhận được vì Srm_Mp3GetDecodeTime() phía bên gửi luôn có timeout + giá trị
 * cũ để dùng tạm, không bị treo.
 *
 * 4 nhánh xử lý theo buttonState:
 * - BTN_STATE_NEXT/PREV/PLAY_NEW: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> huỷ + xả sạch trạng thái decode của bài CŨ (vs1053_cancel_song, ĐÃ TỪNG bắt đầu decode
 *   trước đó - xem lbHasStartedBefore), rồi mới chuẩn bị trạng thái cho bài MỚI
 *   (vs1053_start_song) TRƯỚC KHI gọi Mp3_StreamSong() - bắt buộc phải huỷ bài cũ trước vì đây
 *   là chuyển bài GIỮA CHỪNG (VS1053 có thể đang decode dở 1 frame của bài cũ khi Next/Prev
 *   tới), khác với BTN_STATE_PLAY (chỉ Resume, không có bài cũ nào đang dở cần dọn). Lần
 *   BTN_STATE_PLAY_NEW ĐẦU TIÊN sau boot bỏ qua vs1053_cancel_song() vì chưa từng decode gì.
 *   Sdcard_Task nhận CÙNG notification này để
 *   tự mở file mp3 mới và nạp lại xMp3RingBuffer từ đầu (xem Sdcard_LoadSong trong sdcard.c)
 *   - Mp3_Task không tự đụng tới thẻ SD nữa.
 * - BTN_STATE_PLAY: bấm Play (đầu tiên hoặc Resume sau Pause), bài không đổi -> chạy thẳng
 *   Mp3_StreamSong(), không reset VS1053, tiếp tục rút dữ liệu từ xMp3RingBuffer đúng
 *   vị trí đang dừng.
 * - BTN_STATE_PAUSE: không đổi bài, chỉ tạm dừng -> Mp3_HandlePause() (không đi qua
 *   Mp3_StreamSong() nữa - cùng khuôn mẫu BTN_STATE_PLAY/BTN_STATE_PAUSE của Oled_Task).
 * - Các buttonState còn lại (di chuyển cursor MENU, peek MENU...) -> bỏ qua.
 *
 * Cơ chế lbHasPendingNotify: Mp3_StreamSong()/Mp3_HandlePause() có thể thoát sớm vì
 * vừa nhận 1 notification mới (thay vì thoát vì Pause/hết file) và đã đọc giá trị đó vào
 * lu32button_evt. Nếu vòng lặp ngoài gọi xTaskNotifyWait() lần nữa ngay lúc đó thì giá
 * trị vừa nhận sẽ bị mất (thay bằng giá trị chờ tiếp theo). lbHasPendingNotify = true báo
 * cho vòng lặp biết: bỏ qua bước chờ, xử lý ngay lu32button_evt đang có sẵn ở vòng lặp
 * kế tiếp.
 *
 * @param pvParameters
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Mp3_Task(void *pvParameters)
{
    /* Device VS1053 - dùng instance global gVs1053DeviceInfo (định nghĩa trong
       config/hardware/vs1053_config.c, khởi tạo sẵn với dreq_pin/reset_pin gán theo board)
       thay vì tự khai báo biến local. Chỉ Mp3_Task được phép
       đụng vào (kiến trúc Owner Task) dù biến là global - quy ước, không phải giới hạn
       compiler. XCS/XDCS không phải field của struct này - driver SPI Master tự quản lý qua
       spics_io_num, xem vs1053_add_spi_devices() trong vs1053.c */

    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32button_evt;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32button_evt đã có sẵn
       1 giá trị mới cần xử lý ngay (do Mp3_StreamSong vừa trả về) */
    bool lbHasPendingNotify = false;
    /* true = Mp3_StreamSong() vừa phát hiện VS1053 mất phản hồi giữa chừng (DREQ timeout,
       xem vs1053_send_buffer trong vs1053.c) - kiểm tra ngay sau switch bên dưới mỗi vòng lặp,
       nếu true thì Mp3_Task chuyển sang halt êm (cùng idiom với lỗi vs1053_init() bên dưới)
       thay vì tiếp tục cố gắng stream vào 1 chip không còn phản hồi */
    bool lbHwFailure = false;
    /* true = đã từng thực sự bắt đầu decode ít nhất 1 bài (vs1053_start_song() đã được gọi
       trước đó) - dùng để BỎ QUA vs1053_cancel_song() ở lần BTN_STATE_PLAY_NEW ĐẦU TIÊN sau
       boot: lúc đó VS1053 chưa từng decode gì (vừa vs1053_init() xong), set SM_CANCEL để "huỷ
       bài đang decode dở" trên 1 chip đang rảnh không có gì để huỷ khiến bit này không bao
       giờ tự clear (đã quan sát được trên board thật - "SM_CANCEL not cleared" xảy ra ngay ở
       lần chọn bài đầu tiên), buộc phục hồi bằng reset cứng oan uổng dù chưa hề có lỗi gì */
    bool lbHasStartedBefore = false;

    /* Tạo xMp3CommandQueue/xMp3RingBuffer TRƯỚC vs1053_init() - đây là phần nhanh (chỉ cấp
       phát RAM), làm trước để 2 object này sẵn sàng càng sớm càng tốt, không phải chờ
       vs1053_init() (chậm - GPIO/SPI/reset phần cứng) chạy xong mới có, xem Mp3_Init() */
    Mp3_Init();

    /* vs1053_init() đã tự làm đầy đủ: reset phần cứng, khởi tạo SPI, kiểm tra kết nối,
       cấu hình clock/audio/mode (SM_SDINEW|SM_LINE1) và volume mặc định (50%) - không cần
       gọi thêm vs1053_switch_to_mp3_mode() ngay sau vì nội dung gần như trùng lặp, hàm đó
       chỉ dành cho việc CHUYỂN LẠI chế độ MP3 sau này nếu có nhu cầu (vd sau khi đổi mode
       khác), không cần thiết lúc khởi động lần đầu. */
    if (vs1053_init(&gVs1053DeviceInfo) != ESP_OK)
    {
        /* Không có VS1053/lỗi kết nối -> không có gì để làm, task nằm im (không return -
           1 FreeRTOS task function không được phép return), tránh gọi tiếp các hàm vs1053_*
           khác trên 1 device chưa chắc đã khởi tạo đúng (spi_handle có thể không hợp lệ) */
        ESP_LOGE(TAG, "vs1053_init failed - VS1053 not detected/misconfigured, Mp3_Task halted");
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
            xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Mp3_StreamSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Rẽ nhánh trực tiếp trên buttonState vừa nhận, không qua state trung gian */
        switch ((PlayerManager_ButtonStateType_e)lu32button_evt)
        {
            case BTN_STATE_NEXT:
            case BTN_STATE_PREV:
            case BTN_STATE_PLAY_NEW:
                /* Dọn sạch dữ liệu byte thô còn sót của bài CŨ trong xMp3RingBuffer TRƯỚC
                   TIÊN (càng sớm càng tốt sau khi nhận notify, giảm khả năng Sdcard_Task đã
                   kịp ghi vài byte đầu của bài MỚI vào rồi mới bị xoá nhầm - xem đánh đổi này
                   trong ring_buffer.h). Mp3_Task là bên "nhận" duy nhất của xMp3RingBuffer nên
                   đây là task DUY NHẤT được phép gọi RingBuffer_Reset() - KHÔNG gọi từ
                   Sdcard_Task (bên "gửi") dù trước đây từng làm vậy, xem chi tiết lý do trong
                   ring_buffer.h (RingBuffer_Reset) và sdcard.c (Sdcard_LoadSong) */
                RingBuffer_Reset(xMp3RingBuffer);

                /* Chuyển bài GIỮA CHỪNG - VS1053 có thể đang decode dở 1 frame của bài cũ
                   lúc này, không dọn sạch trước thì dữ liệu bài mới bơm vào ngay sau có thể
                   trộn lẫn với trạng thái decode cũ, gây rè/click ở điểm chuyển bài:
                   1. vs1053_cancel_song(): set SM_CANCEL, gửi end_fill_byte CỦA BÀI CŨ (chưa
                      refresh) + poll SCI_MODE xác nhận bit tự clear đúng theo datasheet - đã
                      tự làm luôn việc xả FIFO, KHÔNG cần gọi thêm vs1053_stop_song() nữa (xem
                      vs1053_cancel_song() trong vs1053.c - có tự phục hồi bằng reset cứng nếu
                      chip không tự clear SM_CANCEL trong giới hạn cho phép)
                   2. vs1053_start_song(): đọc lại end_fill_byte mới, sẵn sàng cho bài MỚI
                   Bỏ qua bước 1 nếu đây là lần bắt đầu decode ĐẦU TIÊN sau boot (xem
                   lbHasStartedBefore) - không có bài nào đang decode dở để huỷ */
                if (lbHasStartedBefore == true)
                {
                    vs1053_cancel_song(&gVs1053DeviceInfo);
                }
                vs1053_start_song(&gVs1053DeviceInfo);
                lbHasStartedBefore = true;
                lbHasPendingNotify = Mp3_StreamSong(&gVs1053DeviceInfo, &lu32button_evt, &lbHwFailure);
                break;

            case BTN_STATE_PLAY:
                /* Không đổi bài nên không cần reset VS1053 -> chạy thẳng stream */
                lbHasPendingNotify = Mp3_StreamSong(&gVs1053DeviceInfo, &lu32button_evt, &lbHwFailure);
                break;

            case BTN_STATE_PAUSE:
                /* Không đổi bài, chỉ tạm dừng -> dừng bơm dữ liệu (xem Mp3_HandlePause)
                   thay vì tiếp tục stream */
                lbHasPendingNotify = Mp3_HandlePause(&lu32button_evt);
                break;

            case BTN_STATE_UP:
            case BTN_STATE_DOWN:
            case BTN_STATE_BACK_MENU:
            case BTN_STATE_IDLE:
            default:
                /* Sự kiện không liên quan tới phát nhạc -> bỏ qua */
                break;
        }

        /* Kiểm tra ngay sau switch, bất kể case nào vừa chạy - lbHwFailure chỉ có thể được
           Mp3_StreamSong() set true (Mp3_HandlePause()/các case còn lại không đụng tới, giữ
           nguyên giá trị false đã gán tại khai báo hoặc từ lần Mp3_StreamSong() gần nhất).
           Halt êm giống hệt lỗi vs1053_init() ở trên - VS1053 mất phản hồi giữa chừng là cùng
           1 loại lỗi phần cứng, chỉ phát hiện muộn hơn (lúc đang stream thay vì lúc khởi động) */
        if (lbHwFailure == true)
        {
            ESP_LOGE(TAG, "VS1053 hardware failure detected, Mp3_Task halted");
            while (1)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}
