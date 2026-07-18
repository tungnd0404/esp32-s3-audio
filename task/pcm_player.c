/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "pcm_player.h"
#include "max98357a.h"
#include "ring_buffer.h"
#include "player_manager.h"
#include "esp_log.h"
#include "esp_timer.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số request tối đa có thể chờ xử lý trong xPcmCommandQueue cùng lúc */
#define PCM_COMMAND_QUEUE_LENGTH   4U

/* Kích thước mỗi lần rút dữ liệu từ xPcmRingBuffer để ghi cho I2S (byte) - lớn hơn hẳn
   VS1053_CHUNK_SIZE cũ (32 byte, giới hạn phần cứng SDI của VS1053) vì I2S không có giới hạn
   tương tự, gom khối lớn hơn giảm số lần gọi RingBuffer_Read()/Max98357a_Write() mỗi giây.
   PHẢI là bội số của 4 (byte/sample-frame, 16-bit stereo) để không lệch kênh trái-phải giữa
   2 lần gọi liên tiếp - 1024 chia hết cho 4 */
#define PCM_CHUNK_SIZE   1024U

/* Thời gian tối đa chờ RingBuffer_Read() mỗi vòng lặp trong Pcm_StreamSong() khi xPcmRingBuffer
   đang tạm rỗng - cùng vai trò MP3_RING_BUFFER_READ_WAIT_MS cũ */
#define PCM_RING_BUFFER_READ_WAIT_MS   50U

/* Thời gian tối đa chờ MỖI LẦN GỌI Max98357a_Write() nội bộ (mili giây - xem giải thích đơn vị
   trong max98357a.h). Đủ rộng so với chu kỳ đầy hàng đợi DMA thực tế (~vài chục ms, xem
   I2S_DMA_DESC_NUM/I2S_DMA_FRAME_NUM trong i2s_config.h) để không báo lỗi giả trong điều kiện
   hoạt động bình thường, nhưng đủ nhỏ để phát hiện treo thật trong thời gian hợp lý */
#define PCM_I2S_WRITE_TIMEOUT_MS   200U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Pcm_Task */
TaskHandle_t xPcmTaskHandle = NULL;

/* Hàng đợi lệnh dùng chung tới Pcm_Task, tạo trong Pcm_Init() vì Pcm_Task là owner */
QueueHandle_t xPcmCommandQueue = NULL;

/* Ring buffer dữ liệu PCM thô, tạo trong Pcm_Init(). Sdcard_Task ghi, Pcm_Task đọc - xem
   giải thích đầy đủ ở khai báo extern trong pcm_player.h */
RingbufHandle_t xPcmRingBuffer = NULL;

/* true = Sdcard_Task đã đọc hết bài hiện tại, không còn gì để nạp thêm vào xPcmRingBuffer -
   xem giải thích đầy đủ ở khai báo extern trong pcm_player.h. Mặc định true (chưa có bài nào
   mở) */
volatile bool gbPcmStreamEof = true;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Tag dùng cho ESP_LOGx trong module này */
static const char *TAG = "PCM";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Pcm_HandleCommand
 * Xử lý 1 request nhận được từ xPcmCommandQueue (xem srm.h - kiến trúc Owner Task): dựa vào
 * cmdId gọi đúng API Max98357a_* tương ứng, luôn trả lời qua Srm_Reply() dù cmdId có hợp lệ hay
 * không (bên gửi đang chờ trong Srm_PcmGetPlayedSamples(), không trả lời thì họ sẽ phải đợi
 * hết timeout mới coi là lỗi, gây chậm không cần thiết).
 * @param pRequest: request nhận được từ xPcmCommandQueue
 * @return
 */
static void Pcm_HandleCommand(const Srm_Message_s *pRequest)
{
    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        case PCM_CMD_GET_PLAYED_SAMPLES:
            /* Max98357a_GetPlayedSamples() trả uint32_t, ghi thẳng vào pRequest->pData - biến
               đích của Srm_PcmGetPlayedSamples() truyền vào (xem srm.c), payload chỉ còn
               mang kết quả E_OK/E_NOT_OK */
            *(uint32_t *)pRequest->pData = Max98357a_GetPlayedSamples();
            Srm_Reply(pRequest, (uint32_t)E_OK);
            break;

        default:
            /* cmdId lạ (chưa định nghĩa xử lý) -> vẫn trả lời E_NOT_OK để bên gửi không phải
               chờ hết timeout, tránh trường hợp SRM_CMD_INVALID lọt qua được lúc gửi */
            Srm_Reply(pRequest, (uint32_t)E_NOT_OK);
            break;
    }
}

/**
 * @brief Pcm_ServicePendingCommand
 * Check non-blocking (timeout = 0) xem xPcmCommandQueue có request nào đang chờ không, có
 * thì xử lý ngay. Gọi mỗi vòng lặp trong Pcm_StreamSong() để PCM_CMD_GET_PLAYED_SAMPLES
 * (sync_frame.c gọi liên tục để đồng bộ animation) được trả lời kịp thời trong lúc đang phát
 * nhạc.
 * LƯU Ý: hàm này KHÔNG được gọi khi Pcm_Task đang rảnh/tạm dừng (đang chờ notify ở vòng lặp
 * ngoài Pcm_Task, xTaskNotifyWait dùng portMAX_DELAY) - lúc đó request tới xPcmCommandQueue
 * sẽ không có ai xử lý cho tới khi có bài phát tiếp theo. Bên gửi (Srm_PcmGetPlayedSamples())
 * đã có cơ chế timeout + trả về giá trị cũ nên không bị treo, nhưng cần biết giới hạn này.
 * @param
 * @return
 */
static void Pcm_ServicePendingCommand(void)
{
    Srm_Message_s lRequest;

    if (xQueueReceive(xPcmCommandQueue, &lRequest, 0) == pdTRUE)
    {
        Pcm_HandleCommand(&lRequest);
    }
}

/**
 * @brief Pcm_StreamSong
 * Rút dữ liệu PCM thô từ xPcmRingBuffer, ghi cho I2S từng khối tối đa PCM_CHUNK_SIZE byte
 * một, cho tới khi Pause/đổi bài/hết bài. KHÔNG tự đọc thẻ SD (Pcm_Task không phải owner thẻ
 * SD - Sdcard_Task tự mở file .pcm và nạp liên tục vào xPcmRingBuffer ngay khi nhận CÙNG
 * notification đổi bài, xem Sdcard_LoadSong trong sdcard.c), ở đây chỉ việc rút ra và phát.
 * Chỉ nên được gọi khi playbackState đang là PLAYBACK_STATE_PLAY - PAUSE xử lý riêng ở
 * Pcm_Task (chỉ gọi Max98357a_Pause(), không đi qua hàm này). Mỗi vòng lặp đều check
 * non-blocking notification để không trễ khi cần xử lý sự kiện khác, kết quả trả về qua tham
 * số ra để vòng lặp ngoài không bị mất giá trị notification vừa nhận - cùng khuôn mẫu
 * Oled_PlayAnimation()/Sdcard_LoadSong() cũ.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi hàm
 *        trả về true
 * @param pbHwFailure: [out] LUÔN được gán (bất kể hàm trả về gì) - true nếu thoát vì
 *        Max98357a_Write() báo lỗi giao tiếp I2S thật sự (KHÁC hết timeout tạm thời - xem
 *        Max98357a_Write() trong max98357a.c), false cho các lý do thoát bình thường
 *        (Pause/đổi bài/hết bài tự nhiên)
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu gọi sai lúc
 *         playbackState không phải PLAY, xPcmRingBuffer chưa được khởi tạo, đã phát hết bài
 *         (EOF), hoặc phát hiện lỗi I2S (xem *pbHwFailure)
 */
static bool Pcm_StreamSong(uint32_t *pu32NotifyValue, bool *pbHwFailure)
{
    *pbHwFailure = false;

    /* Phòng thủ: hàm này chỉ dành cho lúc đang PLAY, không phải nơi xử lý PAUSE */
    if (gsPlayerContext.playbackState != PLAYBACK_STATE_PLAY)
    {
        return false;
    }

    /* Phòng thủ: module chưa init (Pcm_Init() chưa từng gọi) -> không có gì để đọc */
    if (xPcmRingBuffer == NULL)
    {
        return false;
    }

    /* Vòng lặp stream, chạy tới khi có notification mới cần xử lý (Pause/đổi bài/về menu...)
       hoặc hết bài (EOF) */
    while (1)
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Tranh thủ trả lời các request đang chờ (vd sync_frame.c hỏi played_samples) trong
           lúc đang chạy vòng lặp này, xem Pcm_ServicePendingCommand() */
        Pcm_ServicePendingCommand();

        /* Rút tối đa PCM_CHUNK_SIZE byte hiện có trong ring buffer. Nếu ring buffer đang tạm
           rỗng, RingBuffer_Read() tự chờ tối đa PCM_RING_BUFFER_READ_WAIT_MS - khoảng chờ này
           chính là "nhịp nghỉ" của vòng lặp (thay cho vTaskDelay thủ công) */
        uint8_t *lpChunk = NULL;
        size_t lChunkLen = RingBuffer_Read(xPcmRingBuffer, (void **)&lpChunk, PCM_CHUNK_SIZE,
                                            pdMS_TO_TICKS(PCM_RING_BUFFER_READ_WAIT_MS));
        if (lChunkLen == 0)
        {
            if (gbPcmStreamEof == true)
            {
                /* Sdcard_Task đã đọc hết file .pcm VÀ ring buffer cũng đã rút cạn -> hết bài
                   thật sự, dừng chờ sự kiện kế tiếp (đổi bài / bấm Play lại từ đầu...). KHÔNG
                   tự động chuyển bài kế tiếp - khác VS1053 cũ, không cần vs1053_stop_song()
                   xả FIFO gì cả vì I2S không có FIFO decode nội bộ cần xả.
                   Tắt clock I2S ngay (giống Max98357a_Pause()) - nếu để nguyên kênh bật mà
                   không còn Max98357a_Write() nào tiếp theo, DMA có thể lặp lại khối cuối cùng
                   đã phát thay vì im lặng sạch (cùng lý do giải thích ở Max98357a_Pause(),
                   max98357a.h). BTN_STATE_PLAY (Resume) sau đó sẽ tự Max98357a_Resume() lại. */
                if (Max98357a_Pause() != ESP_OK)
                {
                    ESP_LOGW(TAG, "Max98357a_Pause (EOF) failed");
                }
                break;
            }

            /* Ring buffer tạm thời rỗng, Sdcard_Task chưa kịp nạp thêm -> thử lại vòng sau,
               không phải hết bài */
            continue;
        }

        esp_err_t lRet = Max98357a_Write(lpChunk, lChunkLen, PCM_I2S_WRITE_TIMEOUT_MS);
        RingBuffer_ReturnItem(xPcmRingBuffer, lpChunk);

        if (lRet != ESP_OK)
        {
            /* I2S không ghi được (treo/mất kết nối phần cứng thật sự - Max98357a_Write() đã tự
               retry bù timeout tạm thời ở tầng dưới, xem max98357a.c) */
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(lRet));
            *pbHwFailure = true;
            return false;
        }

        /* [DEBUG TẠM] in mỗi ~1s: có ghi được cho I2S không (lu32DbgWrittenBytes), ring buffer
           còn bao nhiêu byte trống (free size lớn dần nếu Sdcard_Task nạp không kịp), và
           played_samples đọc trực tiếp từ Max98357a_GetPlayedSamples() (KHÔNG qua SRM, để loại
           trừ khả năng chính đường SRM/Pcm_ServicePendingCommand là nơi bị lỗi) - nếu
           played_samples KHÔNG tăng dù writtenBytes/s > 0 đều đặn, nghĩa là I2S nhận dữ liệu
           nhưng callback on_sent không fire (nghi vấn cấu hình I2S/driver); nếu writtenBytes/s
           = 0 hoặc rất thấp, nghĩa là Sdcard_Task không nạp kịp/không nạp được gì. XOÁ khối
           debug này (3 biến local + đoạn log) sau khi xác định xong nguyên nhân. */
        static uint32_t lu32DbgLastLogMs = 0U;
        static uint32_t lu32DbgWrittenBytes = 0U;
        lu32DbgWrittenBytes += (uint32_t)lChunkLen;
        uint32_t lu32DbgNowMs = (uint32_t)(esp_timer_get_time() / 1000);
        if ((lu32DbgNowMs - lu32DbgLastLogMs) >= 1000U)
        {
            ESP_LOGW(TAG, "[DEBUG] writtenBytes/s=%u playedSamples=%u ringFreeBytes=%u eof=%d",
                     (unsigned)lu32DbgWrittenBytes, (unsigned)Max98357a_GetPlayedSamples(),
                     (unsigned)RingBuffer_GetFreeSize(xPcmRingBuffer), (int)gbPcmStreamEof);
            lu32DbgWrittenBytes = 0U;
            lu32DbgLastLogMs = lu32DbgNowMs;
        }
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

void Pcm_Init(void)
{
    xPcmCommandQueue = xQueueCreate(PCM_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
    xPcmRingBuffer = RingBuffer_Init(PCM_RING_BUFFER_SIZE);
}

/**
 * @brief Pcm_Task
 * Task owner duy nhất của kênh I2S/MAX98357A. Cùng khuôn thuật toán với Oled_Task/Sdcard_Task/
 * PlayerManager_Task để các task trong hệ thống đọc thống nhất:
 *
 * Cơ chế nhận sự kiện: giống hệt Mp3_Task cũ - "ngủ" (xTaskNotifyWait, portMAX_DELAY) chờ
 * PlayerManager_Task gửi task notification (chính là gsPlayerContext.buttonState).
 *
 * Pcm_Task còn có kênh input thứ 2: xPcmCommandQueue (request/response theo kiến trúc Owner
 * Task, xem srm.h) - sync_frame.c gửi PCM_CMD_GET_PLAYED_SAMPLES vào đây. Kênh này chỉ được
 * xử lý (Pcm_ServicePendingCommand) bên trong Pcm_StreamSong() - tức CHỈ lúc đang thực sự
 * stream nhạc, cùng giới hạn đã biết với Mp3_Task cũ.
 *
 * 4 nhánh xử lý theo buttonState:
 * - BTN_STATE_NEXT/PREV/PLAY_NEW: đổi bài -> reset xPcmRingBuffer + played_samples về 0 rồi
 *   chạy Pcm_StreamSong() cho bài mới. ĐƠN GIẢN HƠN HẲN Mp3_Task cũ (không cần
 *   vs1053_cancel_song()/vs1053_stop_song()/vs1053_start_song() - I2S không có trạng thái
 *   decode dở của 1 chip ngoài cần huỷ/xả, chỉ cần "quên" dữ liệu cũ và bắt đầu ghi dữ liệu
 *   mới).
 * - BTN_STATE_PLAY: bấm Play (đầu tiên hoặc Resume sau Pause) -> Max98357a_Resume() (bật lại
 *   clock I2S đã tắt lúc Pause) rồi chạy Pcm_StreamSong(), tiếp tục rút dữ liệu từ
 *   xPcmRingBuffer đúng vị trí đang dừng.
 * - BTN_STATE_PAUSE: Max98357a_Pause() (tắt clock I2S - im lặng sạch, played_samples tự đứng
 *   yên theo vì không còn callback on_sent nào fire) - không đi qua Pcm_StreamSong() nữa.
 * - Các buttonState còn lại (di chuyển cursor MENU, peek MENU...) -> bỏ qua.
 *
 * @param pvParameters
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Pcm_Task(void *pvParameters)
{
    (void)pvParameters;

    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32button_evt;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32button_evt đã có sẵn 1 giá trị
       mới cần xử lý ngay (do Pcm_StreamSong vừa trả về) */
    bool lbHasPendingNotify = false;
    /* true = Pcm_StreamSong() vừa phát hiện I2S mất phản hồi giữa chừng - kiểm tra ngay sau
       switch bên dưới mỗi vòng lặp, nếu true thì Pcm_Task chuyển sang halt êm (cùng idiom với
       lỗi Max98357a_Init() bên dưới) thay vì tiếp tục cố gắng ghi vào 1 kênh không còn phản hồi */
    bool lbHwFailure = false;

    /* Tạo xPcmCommandQueue/xPcmRingBuffer TRƯỚC Max98357a_Init() - phần nhanh (chỉ cấp phát
       RAM), làm trước để 2 object này sẵn sàng càng sớm càng tốt */
    Pcm_Init();

    if (Max98357a_Init() != ESP_OK)
    {
        /* Không tạo/cấu hình được kênh I2S -> không có gì để làm, task nằm im (không return -
           1 FreeRTOS task function không được phép return), cùng idiom với Mp3_Task khi
           vs1053_init() thất bại trước đây */
        ESP_LOGE(TAG, "Max98357a_Init failed - Pcm_Task halted");
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Vòng lặp chính của task, chạy vô hạn cho tới khi thiết bị tắt nguồn */
    while (1)
    {
        /* Check xem có notify nào đang pending không? nếu không thì chờ notify mới, chờ vô
           thời hạn (portMAX_DELAY) vì task không có việc gì khác để làm */
        if (lbHasPendingNotify == false)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true bên dưới
           nếu Pcm_StreamSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Rẽ nhánh trực tiếp trên buttonState vừa nhận, không qua state trung gian */
        switch ((PlayerManager_ButtonStateType_e)lu32button_evt)
        {
            case BTN_STATE_NEXT:
            case BTN_STATE_PREV:
            case BTN_STATE_PLAY_NEW:
                /* Dọn sạch dữ liệu byte thô còn sót của bài CŨ trong xPcmRingBuffer TRƯỚC
                   TIÊN (càng sớm càng tốt sau khi nhận notify, giảm khả năng Sdcard_Task đã
                   kịp ghi vài byte đầu của bài MỚI vào rồi mới bị xoá nhầm - xem đánh đổi này
                   trong ring_buffer.h). Pcm_Task là bên "nhận" duy nhất của xPcmRingBuffer nên
                   đây là task DUY NHẤT được phép gọi RingBuffer_Reset() */
                RingBuffer_Reset(xPcmRingBuffer);

                /* Bài mới -> mốc đồng bộ animation bắt đầu lại từ 0 (xem
                   SyncFrame_GetFrameIndex(), driver/buffer/sync_frame.c: frameIndex =
                   played_samples / samples_per_frame - reset về 0 ở đây là đủ, không cần gọi
                   gì thêm ở phía sync_frame.c/oled.c) */
                Max98357a_ResetPlayedSamples(0U);

                lbHasPendingNotify = Pcm_StreamSong(&lu32button_evt, &lbHwFailure);
                break;

            case BTN_STATE_PLAY:
                /* Resume sau Pause (hoặc Play lần đầu) - bật lại clock I2S đã tắt lúc Pause,
                   không đổi bài nên không reset played_samples/ring buffer */
                if (Max98357a_Resume() != ESP_OK)
                {
                    ESP_LOGW(TAG, "Max98357a_Resume failed");
                }
                lbHasPendingNotify = Pcm_StreamSong(&lu32button_evt, &lbHwFailure);
                break;

            case BTN_STATE_PAUSE:
                /* Tắt clock I2S - im lặng sạch ngay, played_samples tự đứng yên theo (xem
                   Max98357a_Pause() trong max98357a.c) */
                if (Max98357a_Pause() != ESP_OK)
                {
                    ESP_LOGW(TAG, "Max98357a_Pause failed");
                }
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
           Pcm_StreamSong() set true. Halt êm giống hệt lỗi Max98357a_Init() ở trên */
        if (lbHwFailure == true)
        {
            ESP_LOGE(TAG, "I2S hardware failure detected, Pcm_Task halted");
            while (1)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}
