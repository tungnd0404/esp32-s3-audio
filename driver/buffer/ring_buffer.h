#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "ff.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stddef.h>
#include "freertos/ringbuf.h"

// Kích thước mỗi lần đọc (byte) – tuỳ chỉnh theo dữ liệu thực tế
#define MP3_SIZE      1024
// Kích thước ring buffer (byte)
#define RING_BUFFER_SIZE      4096

typedef struct {
    RingbufHandle_t handle;
} rb_driver_t;

/* Init ring buffer */
rb_driver_t ring_buffer_init(size_t size);

/* Write from task */
BaseType_t ring_buffer_write(rb_driver_t *rb, const void *data, size_t len, TickType_t timeout);

/* Write from ISR */
BaseType_t ring_buffer_write_isr(rb_driver_t *rb, const void *data, size_t len);

/* Read data */
size_t ring_buffer_read(rb_driver_t *rb, void **buf, TickType_t timeout);

/* Return buffer to ring after read */
void ring_buffer_return(rb_driver_t *rb, void *buf);

/* Reset */
void ring_buffer_reset(rb_driver_t *rb);

/* Delete */
void ring_buffer_deinit(rb_driver_t *rb);

#endif