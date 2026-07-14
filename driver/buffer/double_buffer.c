/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include <string.h>
#include "double_buffer.h"
#include "esp_log.h"

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "DOUBLE_BUFFER";

/* Hai vùng đệm chính, luân phiên phục vụ đọc (xem gbUsingA). CHỈ Sdcard_Task đụng vào -
   Oled_Task không còn đọc thẳng mảng này (DoubleBuffer_GetFrame() memcpy ra buffer riêng
   của bên gọi - xem double_buffer.h) nên không cần mutex bảo vệ như thiết kế cũ */
static uint8_t gau8BufferA[CACHE_FRAMES][FRAME_SIZE];
static uint8_t gau8BufferB[CACHE_FRAMES][FRAME_SIZE];

/* Chỉ số frame đầu tiên trong mỗi buffer */
static uint32_t gu32StartA = 0;
static uint32_t gu32StartB = 0;

/* Số frame thực tế đã nạp được vào mỗi buffer (có thể < CACHE_FRAMES ở cuối file) */
static uint32_t gu32CountA = 0;
static uint32_t gu32CountB = 0;

/* Buffer đã sẵn sàng để đọc chưa */
static bool gbReadyA = false;
static bool gbReadyB = false;

/* Buffer nào đang phục vụ DoubleBuffer_GetFrame() */
static bool gbUsingA = true;

/* Con trỏ FILE đang mở (đọc frame.bin) */
static FILE *gpFrameFile = NULL;

/* Tổng số frame của bài hiện tại (kích thước file / FRAME_SIZE) */
static uint32_t gu32TotalFrames = 0;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief DoubleBuffer_LoadInternal
 * Nạp dữ liệu từ gpFrameFile vào 1 buffer cụ thể, bắt đầu từ frame index, tối đa
 * CACHE_FRAMES frame hoặc tới hết file.
 * @param au8Buffer: buffer đích, mảng 2 chiều [CACHE_FRAMES][FRAME_SIZE]
 * @param pu32Start: [out] chỉ số frame đầu tiên nạp được, chỉ ghi khi nạp thành công
 * @param pu32Count: [out] số frame thực tế nạp được, chỉ ghi khi nạp thành công
 * @param index: chỉ số frame bắt đầu cần nạp
 * @return true nếu nạp được ít nhất 1 frame, false nếu lỗi hoặc index đã ngoài phạm vi file
 */
static bool DoubleBuffer_LoadInternal(uint8_t au8Buffer[][FRAME_SIZE], uint32_t *pu32Start,
                                       uint32_t *pu32Count, uint32_t index)
{
    if (gpFrameFile == NULL)
    {
        return false;
    }

    if (index >= gu32TotalFrames)
    {
        return false;
    }

    long lOffset = (long)index * FRAME_SIZE;
    if (fseek(gpFrameFile, lOffset, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "fseek failed at index %lu", index);
        return false;
    }

    uint32_t lu32FramesLoaded = 0;
    for (int li = 0; li < (int)CACHE_FRAMES; li++)
    {
        if ((index + (uint32_t)li) >= gu32TotalFrames)
        {
            break;
        }

        size_t lReadBytes = fread(au8Buffer[li], 1, FRAME_SIZE, gpFrameFile);
        if (lReadBytes != FRAME_SIZE)
        {
            ESP_LOGE(TAG, "fread failed at frame %lu", index + (uint32_t)li);
            return false;
        }

        lu32FramesLoaded++;
    }

    if (lu32FramesLoaded == 0U)
    {
        return false;
    }

    /* Chỉ cập nhật *pu32Start/*pu32Count cùng lúc, và chỉ khi chắc chắn nạp thành công ít
       nhất 1 frame - tránh để lại cặp (start, count) không nhất quán nếu hàm trả về false
       sau khi *pu32Start đã bị ghi đè */
    *pu32Start = index;
    *pu32Count = lu32FramesLoaded;
    return true;
}

/**
 * @brief DoubleBuffer_FlipBuffer
 * Chuyển buffer đang phục vụ đọc (gbUsingA) sang buffer còn lại nếu buffer đó đã sẵn sàng
 * và đang chứa đúng frame cần.
 * @param index: chỉ số frame đang cần đọc
 * @return true nếu đã chuyển và buffer mới đang chứa frame cần, false nếu chưa thể chuyển
 */
static bool DoubleBuffer_FlipBuffer(uint32_t index)
{
    if (gbUsingA == true)
    {
        if (index >= gu32StartA + gu32CountA)
        {
            if ((gbReadyB == true) && (index >= gu32StartB) && (index < gu32StartB + gu32CountB))
            {
                gbUsingA = false;
                ESP_LOGD(TAG, "Flip to buffer B, startB=%lu, countB=%lu", gu32StartB, gu32CountB);
                return true;
            }
        }
    }
    else
    {
        if (index >= gu32StartB + gu32CountB)
        {
            if ((gbReadyA == true) && (index >= gu32StartA) && (index < gu32StartA + gu32CountA))
            {
                gbUsingA = true;
                ESP_LOGD(TAG, "Flip to buffer A, startA=%lu, countA=%lu", gu32StartA, gu32CountA);
                return true;
            }
        }
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

void DoubleBuffer_Init(void)
{
    gu32StartA = 0;
    gu32StartB = 0;
    gu32CountA = 0;
    gu32CountB = 0;
    gbReadyA = false;
    gbReadyB = false;
    gbUsingA = true;
    gpFrameFile = NULL;
    gu32TotalFrames = 0;

    ESP_LOGI(TAG, "Double buffer module initialized");
}

void DoubleBuffer_Open(const char *path)
{
    if (gpFrameFile != NULL)
    {
        fclose(gpFrameFile);
        gpFrameFile = NULL;
    }

    gpFrameFile = fopen(path, "rb");
    if (gpFrameFile == NULL)
    {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return;
    }

    fseek(gpFrameFile, 0, SEEK_END);
    long lSize = ftell(gpFrameFile);
    fseek(gpFrameFile, 0, SEEK_SET);
    gu32TotalFrames = (uint32_t)lSize / FRAME_SIZE;
    ESP_LOGI(TAG, "Opened %s, total frames = %lu", path, gu32TotalFrames);

    /* Nạp buffer A từ frame 0 */
    gbReadyA = DoubleBuffer_LoadInternal(gau8BufferA, &gu32StartA, &gu32CountA, 0);

    /* Nạp buffer B nối tiếp ngay sau buffer A */
    uint32_t lu32StartBufferB = gu32StartA + gu32CountA;
    gbReadyB = DoubleBuffer_LoadInternal(gau8BufferB, &gu32StartB, &gu32CountB, lu32StartBufferB);

    gbUsingA = true;

    if ((gbReadyA == false) && (gbReadyB == false))
    {
        ESP_LOGE(TAG, "Failed to load double buffer for %s", path);
        fclose(gpFrameFile);
        gpFrameFile = NULL;
        gu32TotalFrames = 0;
    }
}

void DoubleBuffer_Close(void)
{
    if (gpFrameFile != NULL)
    {
        fclose(gpFrameFile);
        gpFrameFile = NULL;
    }

    gbReadyA = false;
    gbReadyB = false;
    gu32StartA = 0;
    gu32StartB = 0;
    gu32CountA = 0;
    gu32CountB = 0;
    gbUsingA = true;
    gu32TotalFrames = 0;
}

bool DoubleBuffer_GetFrame(uint32_t index, uint8_t *pOutFrame)
{
    if ((pOutFrame == NULL) || (index >= gu32TotalFrames))
    {
        return false;
    }

    /* Vòng lặp retry (không đệ quy) - thử tìm frame trong buffer hiện có, nếu không có thì
       tự nạp gấp rồi quay lại thử tiếp. Không cần mutex hay SRM nội bộ - hàm này luôn chạy
       trên chính thread của Sdcard_Task (owner duy nhất), không có ai khác tranh chấp */
    while (1)
    {
        if ((gbUsingA == true) && (gbReadyA == true) &&
            (index >= gu32StartA) && (index < gu32StartA + gu32CountA))
        {
            memcpy(pOutFrame, gau8BufferA[index - gu32StartA], FRAME_SIZE);

            /* Đã đọc quá 80% buffer A và buffer B chưa sẵn sàng -> nạp trước ngay bằng lời
               gọi hàm thường (không cần "cờ đã yêu cầu" như thiết kế SRM cũ, vì nạp trước
               diễn ra đồng bộ ngay trong lần gọi này, không có khoảng trễ nào để lặp lại) */
            if ((gbReadyB == false) && ((index - gu32StartA) > (gu32CountA * 8U / 10U)))
            {
                uint32_t lu32LoadIndex = gu32StartA + gu32CountA;
                if (lu32LoadIndex < gu32TotalFrames)
                {
                    gbReadyB = DoubleBuffer_LoadInternal(gau8BufferB, &gu32StartB, &gu32CountB, lu32LoadIndex);
                    ESP_LOGD(TAG, "Preloaded buffer B at index %lu, ready=%d", lu32LoadIndex, (int)gbReadyB);
                }
            }

            return true;
        }

        if ((gbUsingA == false) && (gbReadyB == true) &&
            (index >= gu32StartB) && (index < gu32StartB + gu32CountB))
        {
            memcpy(pOutFrame, gau8BufferB[index - gu32StartB], FRAME_SIZE);

            if ((gbReadyA == false) && ((index - gu32StartB) > (gu32CountB * 8U / 10U)))
            {
                uint32_t lu32LoadIndex = gu32StartB + gu32CountB;
                if (lu32LoadIndex < gu32TotalFrames)
                {
                    gbReadyA = DoubleBuffer_LoadInternal(gau8BufferA, &gu32StartA, &gu32CountA, lu32LoadIndex);
                    ESP_LOGD(TAG, "Preloaded buffer A at index %lu, ready=%d", lu32LoadIndex, (int)gbReadyA);
                }
            }

            return true;
        }

        /* Không có trong buffer đang dùng -> thử chuyển sang buffer còn lại */
        if (DoubleBuffer_FlipBuffer(index) == true)
        {
            continue;
        }

        /* Không có buffer nào chứa frame cần -> nạp gấp ngay vào buffer hiện KHÔNG phục vụ
           đọc (buffer đang dùng để nguyên, không ghi đè dữ liệu đang/sắp đọc) */
        ESP_LOGW(TAG, "Frame %lu not in any buffer, loading urgently", index);

        bool lbLoaded;
        if (gbUsingA == true)
        {
            lbLoaded = DoubleBuffer_LoadInternal(gau8BufferB, &gu32StartB, &gu32CountB, index);
            gbReadyB = lbLoaded;
        }
        else
        {
            lbLoaded = DoubleBuffer_LoadInternal(gau8BufferA, &gu32StartA, &gu32CountA, index);
            gbReadyA = lbLoaded;
        }

        if (lbLoaded == false)
        {
            ESP_LOGE(TAG, "Failed to load frame %lu", index);
            return false;
        }

        /* Chuyển thẳng gbUsingA sang buffer vừa nạp - biết chắc nó đang chứa đúng index cần,
           không dựa vào DoubleBuffer_FlipBuffer() (chỉ xét chiều TIẾN, index >= start+count)
           vì index yêu cầu có thể NHỎ HƠN start của buffer đang dùng (vd
           SyncFrame_GetFrameIndex() lùi lại 1-2 frame do sai số nội suy giữa 2 lần đồng bộ) -
           nếu không chuyển thẳng ở đây, FlipBuffer() sẽ không bao giờ nhận ra buffer vừa nạp,
           khiến vòng lặp nạp gấp lặp lại mãi vào cùng 1 buffer mà không bao giờ đọc được
           (treo vĩnh viễn - Sdcard_Task chạy trên chính thread này, treo ở đây nghĩa là
           không còn ai xử lý xSdCommandQueue/xMp3RingBuffer nữa) */
        gbUsingA = !gbUsingA;

        /* Nạp xong -> quay lại đầu vòng lặp, chắc chắn tìm thấy ngay ở nhánh tương ứng */
    }
}
