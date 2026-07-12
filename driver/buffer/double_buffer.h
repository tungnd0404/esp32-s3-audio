#ifndef DOUBLE_BUFFER_H
#define DOUBLE_BUFFER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Kích thước mỗi frame animation (byte) */
#define FRAME_SIZE      1024U

/* Số frame lưu được trong mỗi buffer (A hoặc B) */
#define CACHE_FRAMES    15U

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief DoubleBuffer_Init
 * Khởi tạo module double buffer (tạo mutex bảo vệ dữ liệu). Phải gọi trước khi dùng bất kỳ
 * hàm nào khác của module này.
 * @param xOwnerCommandQueue: command queue của task sở hữu dữ liệu nguồn (đọc file
 *        frame.bin từ thẻ SD - hiện là xSdCommandQueue của Sdcard_Task, xem sdcard.h).
 *        DoubleBuffer_GetFrame() gửi yêu cầu nạp trước/nạp gấp qua SRM tới đúng queue này
 *        (xem srm.h) - module không tự biết ai là owner, nhận qua tham số để không phải
 *        include ngược lại sdcard.h
 * @return
 */
void DoubleBuffer_Init(QueueHandle_t xOwnerCommandQueue);

/**
 * @brief DoubleBuffer_Open
 * Mở file frame.bin của bài hát mới, nạp đầy 2 buffer A và B ban đầu (đồng bộ, chạy ngay
 * trong task gọi hàm này - hiện luôn được Sdcard_Task gọi lúc đổi bài)
 * @param path: đường dẫn file frame.bin (vd "/sdcard/song1.bin")
 * @return
 */
void DoubleBuffer_Open(const char *path);

/**
 * @brief DoubleBuffer_Close
 * Đóng file frame.bin đang mở, reset toàn bộ trạng thái buffer về ban đầu
 * @param
 * @return
 */
void DoubleBuffer_Close(void);

/**
 * @brief DoubleBuffer_TotalFrames
 * Lấy tổng số frame của file frame.bin đang mở
 * @param
 * @return tổng số frame, 0 nếu module chưa init hoặc chưa mở file nào
 */
uint32_t DoubleBuffer_TotalFrames(void);

/**
 * @brief DoubleBuffer_GetFrame
 * Lấy dữ liệu 1 frame theo chỉ số. Nếu frame chưa có sẵn trong buffer, tự gửi yêu cầu nạp
 * gấp (SDCARD_CMD_LOAD_MISSING_FRAME, qua Srm_SendCommand) tới owner task và chờ tối đa
 * DOUBLE_BUFFER_SRM_TIMEOUT_MS trước khi trả về false. Khi phát hiện buffer đang đọc sắp
 * hết, tự gửi thêm gợi ý nạp trước buffer còn lại (SDCARD_CMD_PRELOAD_BUFFER, cũng qua
 * Srm_SendCommand nhưng owner trả lời ngay lập tức trước khi thực sự nạp - xem
 * Sdcard_HandleCommand trong sdcard.c - nên gần như không chặn dù cùng đi qua API blocking).
 * @param index: chỉ số frame cần lấy (0..DoubleBuffer_TotalFrames()-1)
 * @param pOutFrame: buffer đích nhận dữ liệu, kích thước tối thiểu FRAME_SIZE byte
 * @return true nếu lấy thành công, false nếu index ngoài phạm vi, pOutFrame NULL, hoặc
 *         owner task không phản hồi kịp yêu cầu nạp gấp
 */
bool DoubleBuffer_GetFrame(uint32_t index, uint8_t *pOutFrame);

/**
 * @brief DoubleBuffer_Preload
 * Nạp trước buffer hiện không phục vụ đọc (buffer còn lại so với buffer đang dùng), nối
 * tiếp ngay sau buffer đang dùng. CHỈ được gọi bởi owner task (Sdcard_Task) khi nhận lệnh
 * SDCARD_CMD_PRELOAD_BUFFER qua SRM - không tự tính toán gì thêm, chỉ nạp nếu trước đó đã
 * có yêu cầu nạp trước đang chờ (tránh nạp thừa khi có nhiều gợi ý dồn lại).
 * @param
 * @return true nếu nạp thành công, false nếu không có gì cần nạp hoặc nạp thất bại
 */
bool DoubleBuffer_Preload(void);

/**
 * @brief DoubleBuffer_LoadFrame
 * Nạp gấp đúng frame theo chỉ số yêu cầu vào buffer hiện không phục vụ đọc. CHỈ được gọi
 * bởi owner task (Sdcard_Task) khi nhận lệnh SDCARD_CMD_LOAD_MISSING_FRAME qua SRM.
 * @param index: chỉ số frame cần nạp gấp
 * @return true nếu nạp thành công (frame đã có trong buffer, sẵn sàng cho
 *         DoubleBuffer_GetFrame() đọc lại), false nếu thất bại (index ngoài phạm vi, lỗi
 *         đọc thẻ SD...)
 */
bool DoubleBuffer_LoadFrame(uint32_t index);

#endif /* DOUBLE_BUFFER_H */
