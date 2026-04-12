#ifndef DOUBLE_BUFFER_H
#define DOUBLE_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// Kích thước mỗi frame (byte) – tuỳ chỉnh theo dữ liệu thực tế
#define FRAME_SIZE      1024

// Số frame lưu trong mỗi buffer (15)
#define CACHE_FRAMES    15

// Các bit sự kiện dùng để đồng bộ giữa OLED task và SD task
#define EVT_PRELOAD     (1 << 0)   // Yêu cầu load trước buffer tiếp theo
#define EVT_LOAD_MISS   (1 << 1)   // Yêu cầu load gấp buffer bị thiếu
#define EVT_READY       (1 << 2)   // Buffer đã sẵn sàng (sau khi load xong)

// Khởi tạo module double buffer (phải gọi trước khi dùng)
void double_buffer_init(void);

// Mở file frame.bin và chuẩn bị buffer ban đầu
// path: đường dẫn đến file (VD: "/sdcard/frame.bin")
void double_buffer_open(const char *path);

// Đóng file và giải phóng tài nguyên
void double_buffer_close(void);

// Lấy tổng số frame trong file đã mở
uint32_t double_buffer_total_frames(void);

// Lấy frame theo chỉ số index (0..total_frames-1)
// out_frame: con trỏ đến vùng nhớ đủ chứa FRAME_SIZE byte
// Trả về true nếu thành công, false nếu lỗi (index ngoài phạm vi hoặc timeout)
bool double_buffer_get_frame(uint32_t index, uint8_t *out_frame);

// Hàm dành riêng cho SD task: thực hiện load dữ liệu từ file vào buffer
// Trả về true nếu load thành công, false nếu lỗi
bool sd_task_load_double_buffer(void);

#ifdef __cplusplus
}
#endif

#endif // DOUBLE_BUFFER_H