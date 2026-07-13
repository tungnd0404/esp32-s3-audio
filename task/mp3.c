/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <string.h>
#include "mp3.h"
#include "vs1053.h"
#include "ring_buffer.h"
#include "player_manager.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số request tối đa có thể chờ xử lý trong xMp3CommandQueue cùng lúc */
#define MP3_COMMAND_QUEUE_LENGTH   4U

/* Thời gian tối đa chờ RingBuffer_Read() mỗi vòng lặp trong Mp3_StreamCurrentSong() khi
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

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

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
 * Rút dữ liệu mp3 thô từ xMp3RingBuffer, gửi cho VS1053 từng khối tối đa VS1053_CHUNK_SIZE
 * byte một, cho tới khi Pause/đổi bài/hết bài. KHÔNG tự đọc thẻ SD (Mp3_Task không còn là
 * nơi mở/đọc file mp3 - vi phạm kiến trúc Owner Task vì Sdcard_Task mới là owner duy nhất
 * của thẻ SD, xem srm.h) - Sdcard_Task tự mở file mp3 và nạp liên tục vào xMp3RingBuffer
 * ngay khi nhận CÙNG notification đổi bài (xem Sdcard_LoadCurrentSong trong sdcard.c), Mp3_Task
 * ở đây chỉ việc rút ra và phát. Cùng khuôn mẫu với Oled_PlayAnimation() (oled.c) và
 * Sdcard_LoadCurrentSong() (sdcard.c): mỗi vòng lặp đều check non-blocking notification để
 * không trễ khi cần xử lý sự kiện khác, kết quả trả về qua tham số ra để vòng lặp ngoài
 * không bị mất giá trị notification vừa nhận.
 *
 * @param pDev: con trỏ device VS1053 (đã Mp3_Task khởi tạo)
 * @param bIsNewSong: true nếu đây là bài MỚI (Next/Prev/chọn bài từ MENU) -> reset trạng
 *        thái decode nội bộ của VS1053 cho bài mới (vs1053_start_song). false nếu chỉ là
 *        Resume sau Pause -> không reset, tiếp tục đúng vị trí đang dừng.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi
 *        hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu thoát do
 *         xMp3RingBuffer chưa được khởi tạo, hoặc đã phát hết bài (EOF)
 */
static bool Mp3_StreamCurrentSong(vs1053_handle_t *pDev, bool bIsNewSong, uint32_t *pu32NotifyValue)
{
    if (bIsNewSong == true)
    {
        /* Reset trạng thái stream nội bộ của VS1053 cho bài mới (end_fill_byte...) */
        vs1053_start_song(pDev);
    }

    /* Phòng thủ: module chưa init (Mp3_Init() chưa từng gọi) -> không có gì để đọc */
    if (xMp3RingBuffer == NULL)
    {
        return false;
    }

    /* Còn stream khi: đang ở màn hình PLAYING và playbackState không phải PAUSE.
       Dùng playbackState (tồn tại liên tục) thay vì buttonState (chỉ tồn tại trong
       khoảnh khắc xử lý 1 lần bấm nút rồi tự về IDLE) - giống hệt Oled_PlayAnimation() */
    while ((gsPlayerContext.mainState == MAIN_STATE_PLAYING) && (gsPlayerContext.playbackState != PLAYBACK_STATE_PAUSE))
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

        vs1053_send_buffer(pDev, lpChunk, lChunkLen);
        RingBuffer_ReturnItem(xMp3RingBuffer, lpChunk);
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào, và
 * xMp3RingBuffer để Sdcard_Task nạp dữ liệu mp3 thô vào cho Mp3_Task rút ra phát (xem
 * mp3.h). Gọi trước khi tạo Mp3_Task VÀ trước Sdcard_Init()/Sdcard_Task (Sdcard_Task cần
 * xMp3RingBuffer đã tồn tại ngay từ lần đổi bài đầu tiên - xem thứ tự gọi trong audio.c).
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
 * chỉ được xử lý (Mp3_ServicePendingCommand) bên trong Mp3_StreamCurrentSong() - tức CHỈ
 * lúc đang thực sự stream nhạc. Nếu Mp3_Task đang rảnh (chưa phát bài nào) hoặc đang
 * Pause, request sẽ không có ai trả lời cho tới khi phát nhạc trở lại - đây là giới hạn
 * đã biết, chấp nhận được vì Srm_SendCommand() phía bên gửi luôn có timeout + giá trị cũ
 * để dùng tạm, không bị treo.
 *
 * 3 nhánh xử lý theo buttonState:
 * - BTN_STATE_NEXT/PREV/PLAY_NEW: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> Mp3_StreamCurrentSong(..., bIsNewSong=true, ...) reset VS1053 cho bài mới. Sdcard_Task
 *   nhận CÙNG notification này để tự mở file mp3 mới và nạp lại xMp3RingBuffer từ đầu (xem
 *   Sdcard_LoadCurrentSong trong sdcard.c) - Mp3_Task không tự đụng tới thẻ SD nữa.
 * - BTN_STATE_PLAY/PAUSE: bấm Play/Pause, bài không đổi -> Mp3_StreamCurrentSong(...,
 *   bIsNewSong=false, ...), không reset VS1053 -> nếu playbackState đang là PLAY thì tiếp
 *   tục rút dữ liệu từ xMp3RingBuffer đúng vị trí đang dừng, nếu đang là PAUSE thì vòng lặp
 *   bên trong thoát ngay lập tức (không đọc/gửi gì).
 * - Các buttonState còn lại (di chuyển cursor MENU, peek MENU...) -> bỏ qua.
 *
 * Cơ chế lbHasPendingNotify: Mp3_StreamCurrentSong() có thể thoát sớm vì vừa nhận 1
 * notification mới (thay vì thoát vì Pause/hết file) và đã đọc giá trị đó vào
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
    /* Device VS1053 - biến local của task này, không cần chia sẻ ra ngoài vì Mp3_Task là
       nơi DUY NHẤT được đụng vào phần cứng (kiến trúc Owner Task) */
    vs1053_handle_t lDevice;
    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32button_evt;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32button_evt đã có sẵn
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
            xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Mp3_StreamCurrentSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Rẽ nhánh trực tiếp trên buttonState vừa nhận, không qua state trung gian */
        switch ((PlayerManager_ButtonStateType_e)lu32button_evt)
        {
            case BTN_STATE_NEXT:
            case BTN_STATE_PREV:
            case BTN_STATE_PLAY_NEW:
                lbHasPendingNotify = Mp3_StreamCurrentSong(&lDevice, true, &lu32button_evt);
                break;

            case BTN_STATE_PLAY:
            case BTN_STATE_PAUSE:
                lbHasPendingNotify = Mp3_StreamCurrentSong(&lDevice, false, &lu32button_evt);
                break;

            case BTN_STATE_UP:
            case BTN_STATE_DOWN:
            case BTN_STATE_BACK_MENU:
            case BTN_STATE_IDLE:
            default:
                /* Sự kiện không liên quan tới phát nhạc -> bỏ qua */
                break;
        }
    }
}
