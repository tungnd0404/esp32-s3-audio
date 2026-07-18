/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "animation_config.h"
#include "sync_frame.h"
#include "i2s_config.h"
#include "pcm_player.h"
#include "srm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số sample-frame ứng với đúng 1 frame animation - I2S_SAMPLE_RATE (i2s_config.h) và
   ANIMATION_FPS (animation_config.h) PHẢI chia hết cho nhau để tránh làm tròn lệch dần theo
   thời gian (44100/15 = 2940, chia hết) */
#define SAMPLES_PER_FRAME   (I2S_SAMPLE_RATE / ANIMATION_FPS)

/* Thời gian tối đa chờ Pcm_Task trả lời 1 request played_samples (ms). Nếu Pcm_Task chưa tồn
   tại/quá bận, không được block Oled_Task quá lâu -> hết thời gian thì dùng tạm giá trị cũ */
#define SYNC_FRAME_REQUEST_TIMEOUT_MS   10U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Giá trị played_samples lần đọc thành công gần nhất - CHỈ dùng làm giá trị dự phòng khi
   Srm_PcmGetPlayedSamples() timeout/thất bại (Pcm_Task chưa kịp trả lời), KHÔNG phải nội suy
   như gf32VirtualTime của bản VS1053 cũ - animation "đứng yên đúng 1 nhịp" thay vì nhảy về 0
   trong trường hợp hiếm gặp này */
static uint32_t gu32LastKnownPlayedSamples = 0U;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

void SyncFrame_Init(void)
{
    gu32LastKnownPlayedSamples = 0U;
}

uint32_t SyncFrame_GetFrameIndex(void)
{
    uint32_t lu32PlayedSamples;

    Std_ReturnType lRet = Srm_PcmGetPlayedSamples(&lu32PlayedSamples, pdMS_TO_TICKS(SYNC_FRAME_REQUEST_TIMEOUT_MS));
    if (lRet == E_OK)
    {
        gu32LastKnownPlayedSamples = lu32PlayedSamples;
    }
    /* Pcm_Task chưa tồn tại/quá bận/timeout -> dùng tạm gu32LastKnownPlayedSamples (giá trị
       đọc thành công gần nhất), không có nhánh else nào khác cần xử lý */

    /* [DEBUG TẠM] in mỗi ~1s: SRM round-trip tới Pcm_Task có thành công không (lRet), giá trị
       played_samples nhận được, và frameIndex tính ra - gọi từ Oled_Task nên tách biệt hẳn
       khỏi log [DEBUG] bên pcm_player.c (2 task khác nhau), giúp xác định lỗi nằm ở phía nào:
       nếu lRet luôn E_NOT_OK (timeout) trong khi pcm_player.c vẫn thấy writtenBytes/s > 0 bình
       thường, nghĩa là Pcm_ServicePendingCommand() không được gọi kịp/xMp3CommandQueue có vấn
       đề - lỗi nằm ở đường SRM, không phải ở việc phát nhạc. XOÁ khối debug này (2 biến static
       + đoạn log) sau khi xác định xong nguyên nhân. */
    static uint32_t lu32DbgLastLogMs = 0U;
    uint32_t lu32DbgNowMs = (uint32_t)(esp_timer_get_time() / 1000);
    if ((lu32DbgNowMs - lu32DbgLastLogMs) >= 1000U)
    {
        ESP_LOGW("SYNC_FRAME", "[DEBUG] srmRet=%d playedSamples=%u frameIndex=%u",
                 (int)lRet, (unsigned)gu32LastKnownPlayedSamples,
                 (unsigned)(gu32LastKnownPlayedSamples / SAMPLES_PER_FRAME));
        lu32DbgLastLogMs = lu32DbgNowMs;
    }

    return gu32LastKnownPlayedSamples / SAMPLES_PER_FRAME;
}
