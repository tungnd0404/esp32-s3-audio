/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "animation_config.h"
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

/* Mốc thời gian (ms) của lần đồng bộ decode time gần nhất (lưới poll cố định, mỗi
   SYNC_FRAME_TICK_MS 1 lần, bất kể giá trị đọc được có đổi hay không) */
static uint32_t gu32LastSyncMs = 0;
/* Giá trị thanh ghi SCI_DECODE_TIME lần đọc gần nhất (16-bit, có thể rollover) */
static uint16_t gu16DecodePrev = 0;
/* Tổng số giây đã giải mã, cộng dồn tuyệt đối (không rollover) */
static uint16_t gu16DecodeTotal = 0;
/* Mốc thời gian (ms, wall-clock) của lần gu16DecodeTotal THỰC SỰ tăng gần nhất - khác
   gu32LastSyncMs (mốc lưới poll cố định). SCI_DECODE_TIME chỉ tăng mỗi ~1 giây thật trong
   khi poll mỗi SYNC_FRAME_TICK_MS=100ms, nên đa số các lần poll KHÔNG có gì thay đổi. Nếu
   nội suy phần thập phân theo gu32LastSyncMs (mốc poll) thay vì mốc này, phần thập phân của
   gf32VirtualTime sẽ bị reset về gần 0 ở MỌI lần poll dù giá trị thật chưa đổi, khiến
   frameIndex nhảy lùi định kỳ ~9 lần/giây - đủ để đôi khi lùi qua ranh giới double buffer
   (driver/buffer/double_buffer.c) và gây treo (DoubleBuffer_FlipBuffer chỉ xét chiều tiến) */
static uint32_t gu32DecodeTotalChangedAtMs = 0;
/* Thời gian phát ảo (giây), nội suy mượt giữa 2 lần đồng bộ */
static volatile float gf32VirtualTime = 0.0f;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief SyncFrame_RequestDecodeTime
 * Hỏi Mp3_Task thời gian đã giải mã qua Srm_Mp3GetDecodeTime() (xem srm.h). Response queue
 * do SRM tự quản lý theo task đang gọi - hàm này không cần tự tạo/giữ response queue.
 * @param
 * @return giá trị SCI_DECODE_TIME nhận từ Mp3_Task, hoặc gu16DecodePrev (giá trị cũ) nếu
 *         xMp3CommandQueue chưa tồn tại, đầy, hoặc không nhận được response trong
 *         SYNC_FRAME_REQUEST_TIMEOUT_MS
 */
static uint16_t SyncFrame_RequestDecodeTime(void)
{
    uint16_t lu16DecodeTime;

    if (Srm_Mp3GetDecodeTime(&lu16DecodeTime,
                              pdMS_TO_TICKS(SYNC_FRAME_REQUEST_TIMEOUT_MS)) == E_NOT_OK)
    {
        /* Mp3_Task chưa tồn tại/đầy/timeout -> dùng tạm giá trị cũ */
        return gu16DecodePrev;
    }

    return lu16DecodeTime;
}

/**
 * @brief SyncFrame_UpdateVirtualTime
 * Cập nhật gf32VirtualTime: đọc lại decode time thật mỗi SYNC_FRAME_TICK_MS (xử lý cả
 * rollover 16-bit của thanh ghi), rồi nội suy mượt phần dư kể từ mốc gu16DecodeTotal THỰC
 * SỰ tăng gần nhất (gu32DecodeTotalChangedAtMs) - không phải mốc poll gu32LastSyncMs, vì
 * SCI_DECODE_TIME chỉ tăng mỗi ~1 giây thật trong khi poll mỗi 100ms (xem giải thích ở khai
 * báo gu32DecodeTotalChangedAtMs)
 * @param
 * @return gf32VirtualTime sau khi cập nhật (giây)
 */
static float SyncFrame_UpdateVirtualTime(void)
{
    uint32_t lu32Now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Số ms thực sự đã trôi qua kể từ lần đồng bộ trước - dùng CẢ để quyết định có nên đồng
       bộ lại hay chưa (giữ đúng nhịp poll ~SYNC_FRAME_TICK_MS, không hỏi Mp3_Task dồn dập mỗi
       khi Oled_PlayAnimation gọi hàm này liên tục mỗi ~33ms) LẪN để tính giới hạn delta hợp lệ
       bên dưới - xem giải thích tại sao không dùng vòng lặp "bắt kịp lưới cố định" như bản
       trước nữa. */
    uint32_t lu32ElapsedMs = lu32Now - gu32LastSyncMs;

    if (lu32ElapsedMs >= SYNC_FRAME_TICK_MS)
    {
        /* Nhảy thẳng tới NOW thay vì cộng dồn +SYNC_FRAME_TICK_MS nhiều lần trong 1 vòng lặp
           (bản trước): khi Oled_Task ngừng gọi hàm này một thời gian dài (Pause, hoặc
           double-click peek MENU - nhạc vẫn phát nền nên decode time thật vẫn tăng, xem
           player_manager.c/mp3.c), bản trước phải lặp lại hàng nghìn lần để "trả nợ" các mốc
           bị bỏ lỡ, mỗi lần là 1 round-trip SRM đồng bộ tới Mp3_Task (SyncFrame_RequestDecodeTime)
           - chặn Oled_Task nhiều giây ngay đúng lúc người dùng vừa bấm Play/thoát MENU, mong
           phản hồi tức thì nhất. Chỉ cần đúng 1 lần đọc thanh ghi mỗi lần gọi hàm này là đủ -
           không cần "đi qua" từng mốc trung gian vì không có trạng thái nào cần cập nhật ở
           các mốc đó ngoài delta cộng dồn, và delta đã được tính đúng bằng 1 lần đọc duy nhất
           bên dưới */
        gu32LastSyncMs = lu32Now;

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

        /* Giới hạn delta tối đa THEO ĐÚNG số giây thực đã trôi qua (lu32ElapsedMs/1000 + 1,
           làm tròn lên) - KHÔNG dùng hằng số cố định "1 giây" như bản trước nữa. Hằng số cố
           định chỉ đúng khi giả định luôn poll đều đặn mỗi ~100ms (delta hợp lệ tối đa lúc đó
           đúng là 1); nhưng khi khoảng cách giữa 2 lần gọi lớn hơn nhiều (Pause dài, hoặc peek
           MENU trong khi nhạc vẫn phát nền khiến decode time thật tăng vài giây), hằng số "1"
           sẽ cắt mất phần chênh lệch THẬT SỰ đã xảy ra, khiến gu16DecodeTotal (và do đó
           animation) lệch sau audio VĨNH VIỄN cho tới khi đổi bài (SyncFrame_Init() reset lại
           mốc) - đúng nguyên nhân animation mất đồng bộ sau khi peek MENU. Giới hạn theo elapsed
           wall-clock vẫn giữ nguyên tác dụng chống nhiễu ban đầu (1 lần đọc SPI glitch sinh giá
           trị rác chỉ có thể bị tin tối đa đúng bằng số giây thực đã trôi qua, không hơn) */
        uint32_t lu32MaxDelta = (lu32ElapsedMs / 1000U) + 1U;
        if (lu32MaxDelta > 0xFFFFU)
        {
            lu32MaxDelta = 0xFFFFU;
        }
        if ((uint32_t)lu16DeltaRaw > lu32MaxDelta)
        {
            lu16DeltaRaw = (uint16_t)lu32MaxDelta;
        }

        /* Cập nhật giá trị raw lần trước */
        gu16DecodePrev = lu16RawDecodeTime;

        if (lu16DeltaRaw > 0U)
        {
            /* Giá trị THỰC SỰ vừa tăng -> cộng dồn và dời mốc nội suy. Chỉ dời mốc khi có
               tăng thật - nếu dời ở MỌI lần poll (kể cả không đổi) sẽ gây nhảy lùi frameIndex
               định kỳ, xem giải thích ở khai báo gu32DecodeTotalChangedAtMs */
            gu16DecodeTotal += lu16DeltaRaw;
            gu32DecodeTotalChangedAtMs = gu32LastSyncMs;
        }
    }

    /* Nội suy thời gian ảo kể từ mốc gu16DecodeTotal thực sự tăng gần nhất */
    float lf32DeltaMs = (float)(lu32Now - gu32DecodeTotalChangedAtMs) / 1000.0f;
    if (lf32DeltaMs > 1.0f)
    {
        /* Quá 1 giây thật mà vẫn chưa thấy SCI_DECODE_TIME tăng thêm -> có thể VS1053 đang
           khựng thật (phần cứng) - chặn lại, không để virtual time chạy vượt quá điều counter
           thật đã xác nhận, giữ đúng tinh thần "animation tự khựng theo khi audio khựng" */
        lf32DeltaMs = 1.0f;
    }
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
    gu32DecodeTotalChangedAtMs = gu32LastSyncMs;
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
    uint32_t lu32FrameIndex = (uint32_t)(lf32VirtualTime * ANIMATION_FPS);

    return lu32FrameIndex;
}
