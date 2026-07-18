/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "max98357a.h"
#include "driver/i2s_std.h"
#include "i2s_config.h"
#include "esp_log.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số byte của 1 sample-frame (1 mẫu trái + 1 mẫu phải) - dùng để quy đổi số byte 1 khối DMA
   vừa phát xong (event->size, xem Max98357aOnSentCallback) sang số sample thật. 16-bit
   stereo -> (16/8)*2 = 4 byte/sample-frame */
#define MAX98357A_BYTES_PER_SAMPLE_FRAME   ((I2S_BITS_PER_SAMPLE / 8U) * I2S_CHANNEL_COUNT)

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "MAX98357A";

/* Handle kênh I2S TX duy nhất trong hệ thống - chỉ Pcm_Task (owner, kiến trúc Owner Task, xem
   srm.h) được phép đụng vào các hàm Max98357a_* của file này */
static i2s_chan_handle_t gTxChanHandle = NULL;

/* Số sample-frame ĐÃ THỰC SỰ PHÁT RA DAC - CHỈ được ghi bởi Max98357aOnSentCallback() (chạy
   trong ngữ cảnh ISR, xem giải thích tại đó) và Max98357a_ResetPlayedSamples() (chạy trong
   context task của Pcm_Task). volatile vì được đọc từ Pcm_HandleCommand() (context task,
   khi trả lời PCM_CMD_GET_PLAYED_SAMPLES) trong khi ISR có thể ghi bất kỳ lúc nào - đọc/ghi
   uint32_t aligned trên ESP32-S3 là atomic tự nhiên nên không cần mutex/critical section
   thêm cho riêng biến đơn lẻ này */
static volatile uint32_t gu32PlayedSamples = 0U;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Max98357aOnSentCallback
 * Callback ESP-IDF gọi mỗi khi 1 khối DMA đã được phát xong THẬT SỰ (xác nhận phần cứng qua
 * ngắt, xem i2s_event_callbacks_t.on_sent) - event->size là số byte của đúng khối đó, quy đổi
 * sang sample-frame rồi cộng dồn vào gu32PlayedSamples. Đây CHÍNH LÀ cơ chế đồng bộ animation
 * theo yêu cầu "không dùng timer, dựa hoàn toàn trên số sample thực tế đã phát" - khác hẳn
 * cách đọc bytes_written của i2s_channel_write() (chỉ cho biết đã NẠP vào hàng đợi DMA, chưa
 * chắc đã phát ra thật, trễ tới ~I2S_DMA_DESC_NUM*I2S_DMA_FRAME_NUM/I2S_SAMPLE_RATE giây).
 * CHẠY TRONG NGỮ CẢNH ISR (tài liệu ESP-IDF ghi rõ) - TUYỆT ĐỐI không gọi hàm blocking/log/
 * cấp phát động trong này, chỉ phép toán số nguyên đơn giản.
 * @param handle: handle kênh I2S vừa fire callback (không dùng, hệ thống chỉ có đúng 1 kênh)
 * @param event: dữ liệu sự kiện, event->size = số byte khối DMA vừa phát xong
 * @param user_ctx: không dùng (NULL lúc đăng ký, xem Max98357a_Init)
 * @return false - không cần đánh thức task nào có priority cao hơn (chỉ tăng 1 biến đếm, không
 *         gọi FreeRTOS API nào cần biết xHigherPriorityTaskWoken)
 */
static bool IRAM_ATTR Max98357aOnSentCallback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;

    if (event != NULL)
    {
        gu32PlayedSamples += (uint32_t)(event->size / MAX98357A_BYTES_PER_SAMPLE_FRAME);
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

esp_err_t Max98357a_Init(void)
{
    i2s_chan_config_t lChanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    lChanCfg.dma_desc_num = I2S_DMA_DESC_NUM;
    lChanCfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    /* Chỉ tạo kênh TX (pRxChan = NULL) - project chỉ phát nhạc, không thu âm */
    esp_err_t lRet = i2s_new_channel(&lChanCfg, &gTxChanHandle, NULL);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    /* Chuẩn Philips/I2S chính thống (I2S_STD_MSB_SLOT_DEFAULT_CONFIG - dữ liệu bắt đầu 1 chu
       kỳ BCLK sau cạnh WS) - đúng định dạng MAX98357A yêu cầu, KHÁC left-justified. mclk để
       I2S_GPIO_UNUSED vì MAX98357A không cần (xác nhận qua datasheet), din cũng UNUSED vì
       đây là kênh TX thuần, không thu âm */
    i2s_std_config_t lStdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    lRet = i2s_channel_init_std_mode(gTxChanHandle, &lStdCfg);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    /* Đăng ký on_sent TRƯỚC khi enable kênh - tránh lọt mất sự kiện của (các) khối DMA đầu
       tiên nếu enable xong mà chưa kịp đăng ký callback */
    i2s_event_callbacks_t lCallbacks = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = Max98357aOnSentCallback,
        .on_send_q_ovf = NULL,
    };
    lRet = i2s_channel_register_event_callback(gTxChanHandle, &lCallbacks, NULL);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_register_event_callback failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    lRet = i2s_channel_enable(gTxChanHandle);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    ESP_LOGI(TAG, "I2S audio channel initialized (%u Hz, %u-bit, %u channel(s))",
             (unsigned)I2S_SAMPLE_RATE, (unsigned)I2S_BITS_PER_SAMPLE, (unsigned)I2S_CHANNEL_COUNT);
    return ESP_OK;
}

esp_err_t Max98357a_Write(const uint8_t *pData, size_t len, uint32_t timeoutMsPerAttempt)
{
    size_t lOffset = 0U;

    /* Lặp tới khi ghi đủ len byte - i2s_channel_write() gốc có thể trả về ít hơn len byte nếu
       hết timeoutMsPerAttempt giữa chừng (vd hàng đợi DMA tạm đầy), KHÔNG được coi đó là lỗi
       và bỏ dở phần còn lại (sẽ làm rớt mất 1 đoạn audio thật, gây tiếng click/lệch đồng bộ) */
    while (lOffset < len)
    {
        size_t lBytesWritten = 0U;
        esp_err_t lRet = i2s_channel_write(gTxChanHandle, pData + lOffset, len - lOffset,
                                            &lBytesWritten, timeoutMsPerAttempt);
        if (lRet != ESP_OK)
        {
            return lRet;
        }

        if (lBytesWritten == 0U)
        {
            /* Hết timeout mà không ghi tiến triển được byte nào - coi như treo thật, tránh
               vòng lặp vô hạn không có điểm dừng */
            return ESP_ERR_TIMEOUT;
        }

        lOffset += lBytesWritten;
    }

    return ESP_OK;
}

uint32_t Max98357a_GetPlayedSamples(void)
{
    return gu32PlayedSamples;
}

void Max98357a_ResetPlayedSamples(uint32_t value)
{
    gu32PlayedSamples = value;
}

esp_err_t Max98357a_Pause(void)
{
    return i2s_channel_disable(gTxChanHandle);
}

esp_err_t Max98357a_Resume(void)
{
    return i2s_channel_enable(gTxChanHandle);
}
