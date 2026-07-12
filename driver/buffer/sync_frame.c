/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "config.h"
#include "sync_frame.h"
#include "mp3.h"
#include "srm.h"
#include "esp_timer.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Chu kỳ đọc lại decode time thật từ Mp3_Task (ms), giữa 2 mốc này thời gian được nội suy */
#define SYNC_FRAME_TICK_MS              100U

/* Thời gian tối đa chờ Mp3_Task trả lời 1 request (ms). Nếu Mp3_Task chưa tồn tại/quá bận,
   không được block Oled_Task quá lâu -> hết thời gian thì dùng tạm giá trị cũ */
#define SYNC_FRAME_REQUEST_TIMEOUT_MS   10U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Mốc thời gian (ms) của lần đồng bộ decode time gần nhất */
static uint32_t gu32LastSyncMs = 0;
/* Giá trị thanh ghi SCI_DECODE_TIME lần đọc gần nhất (16-bit, có thể rollover) */
static uint16_t gu16DecodePrev = 0;
/* Tổng số giây đã giải mã, cộng dồn tuyệt đối (không rollover) */
static uint16_t gu16DecodeTotal = 0;
/* Thời gian phát ảo (giây), nội suy mượt giữa 2 lần đồng bộ */
static volatile float gf32VirtualTime = 0.0f;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief SyncFrame_RequestDecodeTime
 * Gửi lệnh MP3_CMD_GET_DECODE_TIME vào xMp3CommandQueue cho Mp3_Task xử lý, chờ response.
 * Response queue do SRM tự quản lý theo task đang gọi (xem Srm_SendCommand) - hàm này
 * không cần tự tạo/giữ response queue nữa.
 * @param
 * @return giá trị SCI_DECODE_TIME nhận từ Mp3_Task, hoặc gu16DecodePrev (giá trị cũ) nếu
 *         xMp3CommandQueue chưa tồn tại, đầy, hoặc không nhận được response trong
 *         SYNC_FRAME_REQUEST_TIMEOUT_MS
 */
static uint16_t SyncFrame_RequestDecodeTime(void)
{
    /* Vào: không cần tham số cho MP3_CMD_GET_DECODE_TIME nên để 0; ra: decode time nhận được */
    uint32_t lu32Payload = 0U;

    if (Srm_SendCommand(xMp3CommandQueue, MP3_CMD_GET_DECODE_TIME, &lu32Payload,
                         pdMS_TO_TICKS(SYNC_FRAME_REQUEST_TIMEOUT_MS)) == false)
    {
        /* Mp3_Task chưa tồn tại/đầy/timeout -> dùng tạm giá trị cũ */
        return gu16DecodePrev;
    }

    return (uint16_t)lu32Payload;
}

/**
 * @brief SyncFrame_UpdateVirtualTime
 * Cập nhật gf32VirtualTime: đọc lại decode time thật mỗi SYNC_FRAME_TICK_MS (xử lý cả
 * rollover 16-bit của thanh ghi), rồi nội suy mượt phần dư trong khoảng chưa tới mốc kế tiếp
 * @param
 * @return gf32VirtualTime sau khi cập nhật (giây)
 */
static float SyncFrame_UpdateVirtualTime(void)
{
    uint32_t lu32Now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Xử lý các mốc SYNC_FRAME_TICK_MS bị bỏ lỡ (chống trượt) */
    while ((lu32Now - gu32LastSyncMs) >= SYNC_FRAME_TICK_MS)
    {
        gu32LastSyncMs += SYNC_FRAME_TICK_MS;

        /* Đọc thanh ghi decode_time (16-bit, tăng mỗi giây, có thể rollover) qua Mp3_Task */
        uint16_t lu16RawDecodeTime = SyncFrame_RequestDecodeTime();

        /* Tính delta (có xử lý rollover 65535 -> 0) */
        uint16_t lu16DeltaRaw;
        if (lu16RawDecodeTime >= gu16DecodePrev)
        {
            lu16DeltaRaw = lu16RawDecodeTime - gu16DecodePrev;
        }
        else
        {
            lu16DeltaRaw = (65535U - gu16DecodePrev) + lu16RawDecodeTime + 1U;
        }

        /* Giới hạn delta tối đa 1 giây (chống nhảy do nhiễu) */
        if (lu16DeltaRaw > 1U)
        {
            lu16DeltaRaw = 1U;
        }

        /* Cập nhật giá trị raw lần trước và tổng số giây cộng dồn tuyệt đối */
        gu16DecodePrev = lu16RawDecodeTime;
        gu16DecodeTotal += lu16DeltaRaw;
    }

    /* Nội suy thời gian ảo trong khoảng SYNC_FRAME_TICK_MS hiện tại */
    float lf32DeltaMs = (float)(lu32Now - gu32LastSyncMs) / 1000.0f;
    gf32VirtualTime = (float)gu16DecodeTotal + lf32DeltaMs;

    return gf32VirtualTime;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief SyncFrame_Init
 * Khởi tạo bộ đồng bộ animation theo thời gian giải mã mp3. Gọi 1 lần khi bắt đầu phát
 * 1 bài mới, trước khi dùng SyncFrame_GetFrameIndex()
 * @param
 * @return
 */
void SyncFrame_Init(void)
{
    gu32LastSyncMs = (uint32_t)(esp_timer_get_time() / 1000);
    gu16DecodePrev = SyncFrame_RequestDecodeTime();
    gu16DecodeTotal = gu16DecodePrev;
    gf32VirtualTime = (float)gu16DecodePrev;
}

/**
 * @brief SyncFrame_GetFrameIndex
 * Tính chỉ số frame animation cần hiển thị tại thời điểm hiện tại, đồng bộ theo thời gian
 * giải mã mp3 thực tế
 * @param
 * @return chỉ số frame animation (bắt đầu từ 0)
 */
uint32_t SyncFrame_GetFrameIndex(void)
{
    float lf32VirtualTime = SyncFrame_UpdateVirtualTime();

    /* Đảm bảo virtual time không âm (phòng trường hợp lỗi) */
    if (lf32VirtualTime < 0.0f)
    {
        lf32VirtualTime = 0.0f;
    }

    /* Chỉ số frame = số giây đã phát * số frame/giây */
    uint32_t lu32FrameIndex = (uint32_t)(lf32VirtualTime * FRAME_PER_SECOND);

    return lu32FrameIndex;
}
