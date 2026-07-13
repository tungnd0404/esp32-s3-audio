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
 * SDCARD_CMD_GET_FRAME từ Oled_Task - xem sdcard.c). Vì chỉ 1 thread duy nhất từng đụng vào
 * dữ liệu module này, không cần mutex bảo vệ như thiết kế cũ. Phải gọi trước
 * DoubleBuffer_Open()/DoubleBuffer_GetFrame().
 * @param
 * @return
 */
void DoubleBuffer_Init(void);

/**
 * @brief DoubleBuffer_SetOutputBuffer
 * Đăng ký buffer đích nhận dữ liệu frame cho DoubleBuffer_GetFrame(). Gọi ĐÚNG 1 LẦN lúc
 * khởi động (từ Oled_Init(), trước khi bất kỳ task nào chạy) bởi bên ĐỌC dữ liệu (Oled_Task)
 * - khác với mọi hàm còn lại trong file này (chỉ Sdcard_Task gọi). Nhờ đăng ký sẵn 1 lần,
 * request SDCARD_CMD_GET_FRAME qua SRM chỉ cần mang đúng 1 giá trị (chỉ số frame) mà không
 * cần mở rộng Srm_Message_s để mang thêm con trỏ đích mỗi lần gọi.
 * @param pOutFrame: buffer đích, kích thước tối thiểu FRAME_SIZE byte, phải còn tồn tại
 *        suốt vòng đời chương trình (Oled_Task truyền buffer static của chính nó)
 * @return
 */
void DoubleBuffer_SetOutputBuffer(uint8_t *pOutFrame);

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
 * Lấy dữ liệu 1 frame theo chỉ số, ghi thẳng vào buffer đã đăng ký qua
 * DoubleBuffer_SetOutputBuffer(). CHỈ được gọi bởi Sdcard_Task (qua Sdcard_HandleCommand khi
 * nhận SDCARD_CMD_GET_FRAME) - vì luôn chạy trên chính thread của Sdcard_Task, hàm tự nạp
 * trước/nạp gấp bằng lời gọi hàm thường (không cần mutex hay round-trip SRM nào khác).
 * @param index: chỉ số frame cần lấy (0..tổng số frame của bài đang mở - 1)
 * @return true nếu lấy thành công, false nếu index ngoài phạm vi, chưa đăng ký buffer đích
 *         (DoubleBuffer_SetOutputBuffer chưa từng gọi), hoặc đọc thẻ SD thất bại
 */
bool DoubleBuffer_GetFrame(uint32_t index);

#endif /* DOUBLE_BUFFER_H */
