/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include "ring_buffer.h"
#include "esp_log.h"

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "RING_BUFFER";

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

RingbufHandle_t RingBuffer_Init(size_t size)
{
    RingbufHandle_t xRingBuf = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
    if (xRingBuf == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ring buffer (size=%u)", (unsigned int)size);
    }

    return xRingBuf;
}

BaseType_t RingBuffer_Write(RingbufHandle_t xRingBuf, const void *pData, size_t size, TickType_t timeoutTicks)
{
    if (xRingBuf == NULL)
    {
        return pdFAIL;
    }

    return xRingbufferSend(xRingBuf, pData, size, timeoutTicks);
}

size_t RingBuffer_Read(RingbufHandle_t xRingBuf, void **ppOutData, size_t wantedSize, TickType_t timeoutTicks)
{
    if ((xRingBuf == NULL) || (ppOutData == NULL))
    {
        return 0;
    }

    size_t lItemSize = 0;

    /* xRingbufferReceiveUpTo (không phải xRingbufferReceive) - giới hạn đúng wantedSize byte
       mỗi lần, xem giải thích trong ring_buffer.h */
    *ppOutData = xRingbufferReceiveUpTo(xRingBuf, &lItemSize, timeoutTicks, wantedSize);
    if (*ppOutData == NULL)
    {
        return 0;
    }

    return lItemSize;
}

void RingBuffer_ReturnItem(RingbufHandle_t xRingBuf, void *pItem)
{
    if ((xRingBuf == NULL) || (pItem == NULL))
    {
        return;
    }

    vRingbufferReturnItem(xRingBuf, pItem);
}

size_t RingBuffer_GetFreeSize(RingbufHandle_t xRingBuf)
{
    if (xRingBuf == NULL)
    {
        return 0;
    }

    return xRingbufferGetCurFreeSize(xRingBuf);
}

void RingBuffer_Reset(RingbufHandle_t xRingBuf)
{
    if (xRingBuf == NULL)
    {
        return;
    }

    void *pItem;
    size_t lItemSize;

    /* Rút cạn thủ công: nhận rồi trả lại ngay từng item cho tới khi ring buffer báo rỗng
       (timeoutTicks = 0, không chờ) */
    while ((pItem = xRingbufferReceiveUpTo(xRingBuf, &lItemSize, 0, SIZE_MAX)) != NULL)
    {
        vRingbufferReturnItem(xRingBuf, pItem);
    }
}

void RingBuffer_Deinit(RingbufHandle_t xRingBuf)
{
    if (xRingBuf == NULL)
    {
        return;
    }

    vRingbufferDelete(xRingBuf);
}
