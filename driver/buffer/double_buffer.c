/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <string.h>
#include "double_buffer.h"
#include "esp_log.h"
#include "srm.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Thời gian tối đa chờ owner task (Sdcard_Task) phản hồi 1 request gửi qua Srm_SendCommand()
   (ms): với SDCARD_CMD_LOAD_MISSING_FRAME là chờ nạp gấp xong thật; với
   SDCARD_CMD_PRELOAD_BUFFER chỉ là chờ owner "nhận lệnh" (ack ngay, xem
   Sdcard_HandleCommand trong sdcard.c) nên trong trường hợp bình thường trả lời gần như
   tức thì, không thực sự chờ tới hết giá trị này */
#define DOUBLE_BUFFER_SRM_TIMEOUT_MS   5000U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "DOUBLE_BUFFER";

/* Hai vùng đệm chính, luân phiên phục vụ đọc (xem gbUsingA) */
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

/* Tránh gửi gợi ý nạp trước (SDCARD_CMD_PRELOAD_BUFFER) nhiều lần cho cùng 1 buffer trong
   lúc owner task chưa kịp xử lý xong yêu cầu trước đó */
static bool gbPreloadRequestedForA = false;
static bool gbPreloadRequestedForB = false;

/* Mutex bảo vệ toàn bộ trạng thái tĩnh phía trên (2 buffer, start/count/ready của mỗi
   buffer, gbUsingA, cờ preload, gpFrameFile, gu32TotalFrames) khỏi truy cập đồng thời giữa
   Oled_Task (đọc, qua DoubleBuffer_GetFrame) và Sdcard_Task (ghi, qua
   DoubleBuffer_Preload/DoubleBuffer_LoadFrame) */
static SemaphoreHandle_t gxMutexDoubleBuffer = NULL;

/* Command queue của owner task (Sdcard_Task) - nhận qua tham số DoubleBuffer_Init(), dùng
   để gửi SDCARD_CMD_PRELOAD_BUFFER/SDCARD_CMD_LOAD_MISSING_FRAME qua SRM. Module này không
   tự biết ai là owner (tránh include ngược lại sdcard.h) */
static QueueHandle_t gxOwnerCommandQueue = NULL;

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
 * CACHE_FRAMES frame hoặc tới hết file. PHẢI được gọi trong lúc đã giữ gxMutexDoubleBuffer.
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
 * và đang chứa đúng frame cần. PHẢI được gọi trong lúc đã giữ gxMutexDoubleBuffer.
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

/**
 * @brief DoubleBuffer_Init
 * Khởi tạo module double buffer (tạo mutex bảo vệ dữ liệu). Phải gọi trước khi dùng bất kỳ
 * hàm nào khác của module này.
 * @param xOwnerCommandQueue: command queue của task sở hữu dữ liệu nguồn (xem double_buffer.h)
 * @return
 */
void DoubleBuffer_Init(QueueHandle_t xOwnerCommandQueue)
{
    gu32StartA = 0;
    gu32StartB = 0;
    gu32CountA = 0;
    gu32CountB = 0;
    gbReadyA = false;
    gbReadyB = false;
    gbUsingA = true;
    gbPreloadRequestedForA = false;
    gbPreloadRequestedForB = false;
    gpFrameFile = NULL;
    gu32TotalFrames = 0;

    gxOwnerCommandQueue = xOwnerCommandQueue;

    gxMutexDoubleBuffer = xSemaphoreCreateMutex();
    if (gxMutexDoubleBuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    ESP_LOGI(TAG, "Double buffer module initialized");
}

/**
 * @brief DoubleBuffer_Open
 * Mở file frame.bin của bài hát mới, nạp đầy 2 buffer A và B ban đầu
 * @param path: đường dẫn file frame.bin
 * @return
 */
void DoubleBuffer_Open(const char *path)
{
    if (gxMutexDoubleBuffer == NULL)
    {
        ESP_LOGE(TAG, "Module not initialized. Call DoubleBuffer_Init() first.");
        return;
    }

    xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);

    if (gpFrameFile != NULL)
    {
        fclose(gpFrameFile);
        gpFrameFile = NULL;
    }

    gpFrameFile = fopen(path, "rb");
    if (gpFrameFile == NULL)
    {
        ESP_LOGE(TAG, "Cannot open %s", path);
        xSemaphoreGive(gxMutexDoubleBuffer);
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
    gbPreloadRequestedForA = false;
    gbPreloadRequestedForB = false;

    if ((gbReadyA == false) && (gbReadyB == false))
    {
        ESP_LOGE(TAG, "Failed to load double buffer for %s", path);
        fclose(gpFrameFile);
        gpFrameFile = NULL;
        gu32TotalFrames = 0;
    }

    xSemaphoreGive(gxMutexDoubleBuffer);
}

/**
 * @brief DoubleBuffer_Close
 * Đóng file frame.bin đang mở, reset toàn bộ trạng thái buffer về ban đầu
 * @param
 * @return
 */
void DoubleBuffer_Close(void)
{
    /* Module chưa init (hoặc init thất bại) -> chưa có mutex để lấy, tránh
       xSemaphoreTake(NULL, ...) là undefined behavior */
    if (gxMutexDoubleBuffer == NULL)
    {
        return;
    }

    xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);

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
    gbPreloadRequestedForA = false;
    gbPreloadRequestedForB = false;

    xSemaphoreGive(gxMutexDoubleBuffer);
}

/**
 * @brief DoubleBuffer_TotalFrames
 * Lấy tổng số frame của file frame.bin đang mở
 * @param
 * @return tổng số frame, 0 nếu module chưa init hoặc chưa mở file nào
 */
uint32_t DoubleBuffer_TotalFrames(void)
{
    if (gxMutexDoubleBuffer == NULL)
    {
        return 0;
    }

    xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);
    uint32_t lu32Ret = gu32TotalFrames;
    xSemaphoreGive(gxMutexDoubleBuffer);
    return lu32Ret;
}

/**
 * @brief DoubleBuffer_GetFrame
 * Lấy dữ liệu 1 frame theo chỉ số, tự nạp gấp qua SRM nếu chưa có sẵn (xem double_buffer.h)
 * @param index: chỉ số frame cần lấy
 * @param pOutFrame: buffer đích, kích thước tối thiểu FRAME_SIZE byte
 * @return true nếu lấy thành công, false nếu lỗi hoặc owner task không phản hồi kịp
 */
bool DoubleBuffer_GetFrame(uint32_t index, uint8_t *pOutFrame)
{
    if (pOutFrame == NULL)
    {
        return false;
    }

    if (index >= DoubleBuffer_TotalFrames())
    {
        ESP_LOGE(TAG, "Index %lu out of range (max %lu)", index, gu32TotalFrames);
        return false;
    }

    /* Vòng lặp retry (không đệ quy) - thử tìm frame trong buffer hiện có, nếu không có thì
       gửi yêu cầu nạp gấp rồi quay lại thử tiếp */
    while (1)
    {
        bool lbFound = false;

        xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);

        if ((gbUsingA == true) && (gbReadyA == true))
        {
            if ((index >= gu32StartA) && (index < gu32StartA + gu32CountA))
            {
                memcpy(pOutFrame, gau8BufferA[index - gu32StartA], FRAME_SIZE);
                lbFound = true;

                /* Đã đọc quá 80% buffer A và chưa gửi gợi ý nạp trước cho B -> gửi ngay.
                   Dùng Srm_SendCommand (không phải notify riêng) để chỉ 1 API gửi lệnh duy
                   nhất cho mọi request tới Sdcard_Task - Sdcard_Task trả lời NGAY (ack) khi
                   nhận được lệnh này, TRƯỚC KHI thực sự nạp (xem Sdcard_HandleCommand), nên
                   lời gọi dưới đây thường trả về gần như tức thì, không phải chờ tới lúc
                   nạp xong thật mới được trả lời - không có rủi ro deadlock vì owner task
                   chỉ lấy mutex SAU khi đã trả lời xong, không phải trước */
                if ((gbPreloadRequestedForB == false) && ((index - gu32StartA) > (gu32CountA * 8U / 10U)))
                {
                    gbPreloadRequestedForB = true;
                    uint32_t lu32PreloadAck = 0U;
                    Srm_SendCommand(gxOwnerCommandQueue, SDCARD_CMD_PRELOAD_BUFFER, &lu32PreloadAck,
                                     pdMS_TO_TICKS(DOUBLE_BUFFER_SRM_TIMEOUT_MS));
                    ESP_LOGD(TAG, "Preload requested for buffer B at index %lu", index);
                }
            }
        }
        else if ((gbUsingA == false) && (gbReadyB == true))
        {
            if ((index >= gu32StartB) && (index < gu32StartB + gu32CountB))
            {
                memcpy(pOutFrame, gau8BufferB[index - gu32StartB], FRAME_SIZE);
                lbFound = true;

                if ((gbPreloadRequestedForA == false) && ((index - gu32StartB) > (gu32CountB * 8U / 10U)))
                {
                    gbPreloadRequestedForA = true;
                    uint32_t lu32PreloadAck = 0U;
                    Srm_SendCommand(gxOwnerCommandQueue, SDCARD_CMD_PRELOAD_BUFFER, &lu32PreloadAck,
                                     pdMS_TO_TICKS(DOUBLE_BUFFER_SRM_TIMEOUT_MS));
                    ESP_LOGD(TAG, "Preload requested for buffer A at index %lu", index);
                }
            }
        }

        if (lbFound == false)
        {
            /* Không có trong buffer đang dùng -> thử chuyển sang buffer còn lại */
            if (DoubleBuffer_FlipBuffer(index) == true)
            {
                xSemaphoreGive(gxMutexDoubleBuffer);
                continue;
            }
        }

        xSemaphoreGive(gxMutexDoubleBuffer);

        if (lbFound == true)
        {
            return true;
        }

        /* Không có buffer nào chứa frame cần -> gửi yêu cầu nạp gấp qua SRM (blocking, có
           timeout). KHÔNG được giữ mutex trong lúc chờ phản hồi - owner task (Sdcard_Task)
           cần tự lấy mutex để ghi vào buffer khi xử lý yêu cầu này, giữ mutex ở đây trong
           lúc chờ sẽ gây deadlock giữa 2 task */
        ESP_LOGW(TAG, "Frame %lu not in any buffer, requesting urgent load", index);

        uint32_t lu32Payload = index;
        bool lbSent = Srm_SendCommand(gxOwnerCommandQueue, SDCARD_CMD_LOAD_MISSING_FRAME,
                                       &lu32Payload, pdMS_TO_TICKS(DOUBLE_BUFFER_SRM_TIMEOUT_MS));
        if ((lbSent == false) || (lu32Payload == 0U))
        {
            ESP_LOGE(TAG, "Owner task failed to load frame %lu in time", index);
            return false;
        }

        /* Owner task báo đã nạp xong -> quay lại đầu vòng lặp, thử tìm frame lần nữa */
    }
}

/**
 * @brief DoubleBuffer_Preload
 * Nạp trước buffer hiện không phục vụ đọc, chỉ gọi bởi owner task khi nhận
 * SDCARD_CMD_PRELOAD_BUFFER (xem double_buffer.h)
 * @param
 * @return true nếu nạp thành công, false nếu không có gì cần nạp hoặc nạp thất bại
 */
bool DoubleBuffer_Preload(void)
{
    if (gpFrameFile == NULL)
    {
        return false;
    }

    xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);

    bool lbNeedLoadA = false;
    bool lbNeedLoadB = false;
    uint32_t lu32LoadIndex = 0;

    if (gbUsingA == true)
    {
        /* Buffer A đang dùng, cần nạp trước buffer B */
        if ((gbReadyB == false) && (gbPreloadRequestedForB == true))
        {
            lbNeedLoadB = true;
            lu32LoadIndex = gu32StartA + gu32CountA;
            if (lu32LoadIndex >= gu32TotalFrames)
            {
                /* Hết file, không còn gì để nạp thêm */
                gbPreloadRequestedForB = false;
                lbNeedLoadB = false;
            }
        }
    }
    else
    {
        /* Buffer B đang dùng, cần nạp trước buffer A */
        if ((gbReadyA == false) && (gbPreloadRequestedForA == true))
        {
            lbNeedLoadA = true;
            lu32LoadIndex = gu32StartB + gu32CountB;
            if (lu32LoadIndex >= gu32TotalFrames)
            {
                gbPreloadRequestedForA = false;
                lbNeedLoadA = false;
            }
        }
    }

    bool lbResult = false;
    if (lbNeedLoadA == true)
    {
        lbResult = DoubleBuffer_LoadInternal(gau8BufferA, &gu32StartA, &gu32CountA, lu32LoadIndex);
        if (lbResult == true)
        {
            gbReadyA = true;
            gbPreloadRequestedForA = false;
            ESP_LOGI(TAG, "Preloaded buffer A at index %lu, count=%lu", lu32LoadIndex, gu32CountA);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to preload buffer A at index %lu", lu32LoadIndex);
        }
    }
    else if (lbNeedLoadB == true)
    {
        lbResult = DoubleBuffer_LoadInternal(gau8BufferB, &gu32StartB, &gu32CountB, lu32LoadIndex);
        if (lbResult == true)
        {
            gbReadyB = true;
            gbPreloadRequestedForB = false;
            ESP_LOGI(TAG, "Preloaded buffer B at index %lu, count=%lu", lu32LoadIndex, gu32CountB);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to preload buffer B at index %lu", lu32LoadIndex);
        }
    }

    xSemaphoreGive(gxMutexDoubleBuffer);
    return lbResult;
}

/**
 * @brief DoubleBuffer_LoadFrame
 * Nạp gấp đúng frame theo chỉ số yêu cầu vào buffer hiện không phục vụ đọc, chỉ gọi bởi
 * owner task khi nhận SDCARD_CMD_LOAD_MISSING_FRAME (xem double_buffer.h)
 * @param index: chỉ số frame cần nạp gấp
 * @return true nếu nạp thành công, false nếu thất bại
 */
bool DoubleBuffer_LoadFrame(uint32_t index)
{
    if (gpFrameFile == NULL)
    {
        return false;
    }

    xSemaphoreTake(gxMutexDoubleBuffer, portMAX_DELAY);

    /* Nạp vào buffer hiện KHÔNG phục vụ đọc - buffer đang dùng có thể đang được
       DoubleBuffer_GetFrame() đọc dở (dù thực tế lúc gọi tới đây thì cả 2 buffer đều đã
       được xác nhận không chứa frame cần, xem DoubleBuffer_GetFrame), không được ghi đè */
    bool lbResult;
    if (gbUsingA == true)
    {
        lbResult = DoubleBuffer_LoadInternal(gau8BufferB, &gu32StartB, &gu32CountB, index);
        if (lbResult == true)
        {
            gbReadyB = true;
            gbPreloadRequestedForB = false;
            ESP_LOGI(TAG, "Loaded missing frame %lu into buffer B, count=%lu", index, gu32CountB);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to load missing frame %lu into buffer B", index);
        }
    }
    else
    {
        lbResult = DoubleBuffer_LoadInternal(gau8BufferA, &gu32StartA, &gu32CountA, index);
        if (lbResult == true)
        {
            gbReadyA = true;
            gbPreloadRequestedForA = false;
            ESP_LOGI(TAG, "Loaded missing frame %lu into buffer A, count=%lu", index, gu32CountA);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to load missing frame %lu into buffer A", index);
        }
    }

    xSemaphoreGive(gxMutexDoubleBuffer);
    return lbResult;
}
