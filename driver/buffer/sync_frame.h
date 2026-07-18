#ifndef SYNC_FRAME_H
#define SYNC_FRAME_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief SyncFrame_Init
 * Reset mốc cache nội bộ (gu32LastKnownPlayedSamples, xem sync_frame.c) về 0 - gọi 1 lần khi
 * bắt đầu phát 1 bài mới, trước khi dùng SyncFrame_GetFrameIndex(). KHÔNG bắt buộc phải gọi
 * (Pcm_Task đã tự Max98357a_ResetPlayedSamples(0) khi đổi bài, xem pcm_player.c) - giữ lại chủ
 * yếu để oled.c không cần đổi lời gọi đang có, và để cache không hiện giá trị cũ của bài
 * trước trong 1 khung hình ngắn ngủi trước khi lần gọi GetFrameIndex() đầu tiên tới.
 * @param
 * @return
 */
void SyncFrame_Init(void);

/**
 * @brief SyncFrame_GetFrameIndex
 * Tính chỉ số frame animation cần hiển thị tại thời điểm hiện tại: frameIndex = played_samples
 * / samples_per_frame, với played_samples lấy trực tiếp từ Pcm_Task qua
 * Srm_PcmGetPlayedSamples() (số sample ĐÃ THỰC SỰ PHÁT ra DAC, xác nhận qua callback DMA -
 * xem driver/max98357a/max98357a.c) - KHÔNG còn nội suy/ước lượng theo thời gian như bản
 * VS1053 cũ (SCI_DECODE_TIME chỉ có độ phân giải 1 giây).
 * @param
 * @return chỉ số frame animation (bắt đầu từ 0)
 */
uint32_t SyncFrame_GetFrameIndex(void);

#endif /* SYNC_FRAME_H */
