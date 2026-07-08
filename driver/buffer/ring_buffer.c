#include "ring_buffer.h"
#include "esp_log.h"

static const char *TAG = "RB_DRV";

/* Init */
rb_driver_t ring_buffer_init(size_t size)
{
    rb_driver_t rb = {0};

    rb.handle = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);

    if (rb.handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
    }

    return rb;
}

/* Write from task */
BaseType_t ring_buffer_write(rb_driver_t *rb, const void *data, size_t len, TickType_t timeout)
{
    if (!rb || !rb->handle) return pdFAIL;

    return xRingbufferSend(rb->handle, data, len, timeout);
}

/* Write from ISR */
BaseType_t ring_buffer_write_isr(rb_driver_t *rb, const void *data, size_t len)
{
    if (!rb || !rb->handle) return pdFAIL;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    BaseType_t ret = xRingbufferSendFromISR(
        rb->handle,
        data,
        len,
        &xHigherPriorityTaskWoken
    );

    return ret;
}

/* Read */
size_t ring_buffer_read(rb_driver_t *rb, void **buf, TickType_t timeout)
{
    if (!rb || !rb->handle) return 0;

    size_t item_size = 0;

    *buf = xRingbufferReceive(rb->handle, &item_size, timeout);

    return item_size;
}

/* Return buffer after processing */
void ring_buffer_return(rb_driver_t *rb, void *buf)
{
    if (!rb || !rb->handle || !buf) return;

    vRingbufferReturnItem(rb->handle, buf);
}

/* Reset buffer */
void ring_buffer_reset(rb_driver_t *rb)
{
    if (!rb || !rb->handle) return;

    xRingbufferReset(rb->handle);
}

/* Delete */
void ring_buffer_deinit(rb_driver_t *rb)
{
    if (!rb || !rb->handle) return;

    vRingbufferDelete(rb->handle);
    rb->handle = NULL;
}

bool ring_buffer_open(const char *path)
{
    memset(&mp3_rb, 0, sizeof(mp3_rb));

    FRESULT res = f_open(&mp3_rb.file, path, FA_READ);
    if (res != FR_OK) {
        return false;
    }

    uint32_t total_read = 0;

    while (total_read < RING_BUFFER_SIZE) {

        UINT br = 0;
        uint8_t temp[MP3_SIZE];

        uint32_t to_read = MP3_SIZE;

        if (RING_BUFFER_SIZE - total_read < MP3_SIZE) {
            to_read = RING_BUFFER_SIZE - total_read;
        }

        res = f_read(&mp3_rb.file, temp, to_read, &br);
        if (res != FR_OK || br == 0) {
            mp3_rb.file_end = true;
            break;
        }

        ring_write(temp, br);
        total_read += br;
    }

    return true;
}

void sd_task_load_ring_buffer(void)
{
    if (mp3_rb.file_end) {
        return;
    }

    // nếu còn đủ chỗ cho 1 chunk mới
    if ((RING_BUFFER_SIZE - mp3_rb.data_size) < MP3_SIZE) {
        return;
    }

    uint8_t temp[MP3_SIZE];

    UINT br = 0;

    FRESULT res = f_read(&mp3_rb.file, temp, MP3_SIZE, &br);

    if (res != FR_OK || br == 0) {
        mp3_rb.file_end = true;
        return;
    }

    ring_write(temp, br);
}