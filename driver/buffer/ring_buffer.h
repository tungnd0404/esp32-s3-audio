#ifndef RING_BUFFER_H
#define RING_BUFFER_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief RingBuffer_Init
 * Tạo 1 ring buffer kiểu byte stream (RINGBUF_TYPE_BYTEBUF - không có header/metadata cho
 * từng lần ghi, phù hợp cho dữ liệu dạng luồng byte liên tục như mp3, khác với
 * RINGBUF_TYPE_NOSPLIT dùng cho dữ liệu tách rời từng "item"). Driver thuần, không biết gì
 * về nội dung dữ liệu bên trong - domain nào cần dùng (vd luồng mp3, xem mp3.h/sdcard.c) tự
 * quyết định kích thước và ý nghĩa dữ liệu.
 * @param size: kích thước ring buffer (byte)
 * @return handle của ring buffer vừa tạo, NULL nếu tạo thất bại (hết heap)
 */
RingbufHandle_t RingBuffer_Init(size_t size);

/**
 * @brief RingBuffer_Write
 * Ghi dữ liệu vào ring buffer, gọi từ context task (không phải ISR).
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @param pData: dữ liệu cần ghi
 * @param size: kích thước dữ liệu cần ghi (byte)
 * @param timeoutTicks: thời gian tối đa chờ nếu ring buffer chưa đủ chỗ trống
 * @return pdTRUE nếu ghi thành công, pdFAIL nếu xRingBuf NULL hoặc hết thời gian chờ
 */
BaseType_t RingBuffer_Write(RingbufHandle_t xRingBuf, const void *pData, size_t size, TickType_t timeoutTicks);

/**
 * @brief RingBuffer_Read
 * Đọc tối đa wantedSize byte hiện có trong ring buffer. Dùng xRingbufferReceiveUpTo() (không
 * phải xRingbufferReceive()) để đảm bảo KHÔNG BAO GIỜ trả về nhiều hơn wantedSize byte trong
 * 1 lần đọc - với ring buffer kiểu byte stream, dữ liệu có thể bị chia làm 2 đoạn khi cuộn
 * qua cuối vùng nhớ vật lý, xRingbufferReceiveUpTo() tự trả về đúng phần liên tục đầu tiên
 * (có thể ít hơn wantedSize), gọi lại lần nữa ở vòng lặp sau để lấy phần còn lại - bên gọi
 * (Mp3_Task) vốn đã lặp liên tục nên không cần xử lý gì thêm cho trường hợp này.
 * Dữ liệu trả về (*ppOutData) vẫn nằm trong vùng nhớ nội bộ của ring buffer - PHẢI gọi
 * RingBuffer_ReturnItem() sau khi dùng xong để giải phóng lại chỗ đó cho lần ghi sau.
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @param ppOutData: [out] con trỏ tới dữ liệu đọc được, chỉ hợp lệ khi hàm trả về > 0
 * @param wantedSize: số byte tối đa muốn đọc trong lần gọi này
 * @param timeoutTicks: thời gian tối đa chờ nếu ring buffer đang rỗng
 * @return số byte thực tế đọc được (0 nếu xRingBuf/ppOutData NULL hoặc hết thời gian chờ)
 */
size_t RingBuffer_Read(RingbufHandle_t xRingBuf, void **ppOutData, size_t wantedSize, TickType_t timeoutTicks);

/**
 * @brief RingBuffer_ReturnItem
 * Trả lại vùng nhớ vừa đọc được từ RingBuffer_Read() cho ring buffer, giải phóng chỗ đó để
 * nhận dữ liệu ghi mới. BẮT BUỘC gọi đúng 1 lần cho mỗi lần RingBuffer_Read() thành công.
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @param pItem: con trỏ dữ liệu nhận được từ RingBuffer_Read() (tham số ppOutData)
 * @return
 */
void RingBuffer_ReturnItem(RingbufHandle_t xRingBuf, void *pItem);

/**
 * @brief RingBuffer_GetFreeSize
 * Lấy số byte trống còn lại trong ring buffer - dùng để biết trước có nên ghi thêm hay
 * không, tránh RingBuffer_Write() phải chờ hết timeoutTicks vô ích khi buffer đã gần đầy.
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @return số byte trống hiện có, 0 nếu xRingBuf NULL
 */
size_t RingBuffer_GetFreeSize(RingbufHandle_t xRingBuf);

/**
 * @brief RingBuffer_Reset
 * Xoá toàn bộ dữ liệu đang có trong ring buffer (dùng khi đổi bài - dữ liệu cũ còn sót lại
 * không còn ý nghĩa). Rút cạn thủ công từng item (nhận rồi trả lại ngay) thay vì gọi thẳng
 * 1 API reset có sẵn, để không phụ thuộc phiên bản ESP-IDF cụ thể.
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @return
 */
void RingBuffer_Reset(RingbufHandle_t xRingBuf);

/**
 * @brief RingBuffer_Deinit
 * Huỷ ring buffer, giải phóng vùng nhớ đã cấp phát trong RingBuffer_Init().
 * @param xRingBuf: handle ring buffer, lấy từ RingBuffer_Init()
 * @return
 */
void RingBuffer_Deinit(RingbufHandle_t xRingBuf);

#endif /* RING_BUFFER_H */
