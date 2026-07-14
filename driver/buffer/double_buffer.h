#ifndef DOUBLE_BUFFER_H
#define DOUBLE_BUFFER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>

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
 * Khởi tạo module double buffer (reset toàn bộ trạng thái buffer về ban đầu). CHỈ được gọi
 * bởi Sdcard_Task - đây là owner duy nhất của module này, mọi hàm khác trong file này cũng
 * chỉ Sdcard_Task được gọi (trực tiếp, hoặc qua Sdcard_HandleCommand khi nhận
 * SDCARD_CMD_GET_SINGLE_FRAME từ Oled_Task - xem sdcard.c). Vì chỉ 1 thread duy nhất từng đụng vào
 * dữ liệu module này, không cần mutex bảo vệ như thiết kế cũ. Phải gọi trước
 * DoubleBuffer_Open()/DoubleBuffer_GetFrame().
 * @param
 * @return
 */
void DoubleBuffer_Init(void);

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
 * @brief DoubleBuffer_GetFrame
 * Lấy dữ liệu 1 frame theo chỉ số, ghi thẳng vào pOutFrame. CHỈ được gọi bởi Sdcard_Task
 * (qua Sdcard_HandleCommand khi nhận SDCARD_CMD_GET_SINGLE_FRAME, pOutFrame chính là
 * Srm_Message_s.pData mà Oled_Task truyền vào qua Srm_SdcardGetSingleFrame() - xem srm.h) - vì
 * luôn chạy trên chính thread của Sdcard_Task, hàm tự nạp trước/nạp gấp bằng lời gọi hàm
 * thường (không cần mutex hay round-trip SRM nào khác).
 * @param index: chỉ số frame cần lấy (0..tổng số frame của bài đang mở - 1)
 * @param pOutFrame: buffer đích nhận dữ liệu, kích thước tối thiểu FRAME_SIZE byte
 * @return true nếu lấy thành công, false nếu index ngoài phạm vi, pOutFrame NULL, hoặc đọc
 *         thẻ SD thất bại
 */
bool DoubleBuffer_GetFrame(uint32_t index, uint8_t *pOutFrame);

#endif /* DOUBLE_BUFFER_H */
