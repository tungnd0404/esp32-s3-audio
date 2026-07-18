#ifndef MAX98357A_H
#define MAX98357A_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Max98357a_Init
 * Tạo và cấu hình kênh I2S TX (chuẩn Philips/MSB, xem I2S_STD_MSB_SLOT_DEFAULT_CONFIG) theo
 * đúng chân/tốc độ khai báo trong i2s_config.h, đăng ký callback on_sent để tự đếm số sample
 * ĐÃ THỰC SỰ PHÁT RA (không phải chỉ mới nạp vào hàng đợi DMA - xem Max98357a_GetPlayedSamples),
 * rồi bật kênh (i2s_channel_enable()) sẵn sàng nhận dữ liệu qua Max98357a_Write(). Gọi đúng 1
 * lần bởi chính Pcm_Task lúc khởi động (đầu Pcm_Task, trước khi stream bài đầu tiên) - giống
 * khuôn mẫu vs1053_init() cũ, KHÔNG gọi từ app_main() vì I2S không phải bus dùng chung cho
 * nhiều thiết bị như I2C/SPI trước đây (chỉ Pcm_Task dùng), tự sở hữu trọn vòng đời của mình.
 * @param
 * @return ESP_OK nếu tạo + cấu hình + bật kênh thành công, mã lỗi esp_err_t khác nếu bất kỳ
 *         bước nào thất bại (vd chân GPIO không hợp lệ) - Pcm_Task tự halt êm nếu nhận lỗi,
 *         cùng idiom với vs1053_init() thất bại trước đây
 */
esp_err_t Max98357a_Init(void);

/**
 * @brief Max98357a_Write
 * Ghi dữ liệu PCM thô (16-bit stereo, xem i2s_config.h) vào kênh I2S TX, tự lặp lại tới khi
 * ghi hết đúng len byte hoặc gặp lỗi thật sự - KHÁC i2s_channel_write() gốc (có thể trả về ít
 * hơn len byte nếu hết timeout giữa chừng), đảm bảo bên gọi (Pcm_Task) không bao giờ mất dở
 * dữ liệu 1 chunk chỉ vì ghi chưa xong hết trong 1 lần gọi driver.
 * @param pData: buffer PCM cần ghi
 * @param len: số byte cần ghi (PHẢI là bội số của (I2S_BITS_PER_SAMPLE/8)*I2S_CHANNEL_COUNT,
 *        tức 4 byte/sample-frame với cấu hình 16-bit stereo, nếu không sẽ lệch kênh trái-phải)
 * @param timeoutMsPerAttempt: thời gian tối đa chờ MỖI LẦN GỌI i2s_channel_write() nội bộ
 *        (mili giây - ĐÚNG đơn vị API gốc ESP-IDF, KHÔNG phải tick FreeRTOS như phần lớn API
 *        khác trong project, cẩn thận khi gọi từ Pcm_Task)
 * @return ESP_OK nếu ghi đủ len byte thành công, mã lỗi esp_err_t khác nếu lỗi thật sự hoặc
 *         không tiến triển được (hết timeout mà driver báo 0 byte ghi được)
 */
esp_err_t Max98357a_Write(const uint8_t *pData, size_t len, uint32_t timeoutMsPerAttempt);

/**
 * @brief Max98357a_GetPlayedSamples
 * Số sample-frame (1 sample-frame = 1 mẫu trái + 1 mẫu phải với cấu hình stereo) ĐÃ THỰC SỰ
 * PHÁT RA DAC tính từ lần Max98357a_ResetPlayedSamples() gần nhất - tăng dần bởi callback
 * on_sent (xem max98357a.c), mỗi lần tăng đúng bằng số sample của 1 khối DMA vừa phát xong
 * THẬT SỰ (xác nhận phần cứng qua ngắt DMA, KHÔNG phải suy đoán từ thời gian trôi qua hay số
 * byte mới ghi vào hàng đợi). Đây là nguồn sự thật duy nhất để tính frameIndex animation (xem
 * SyncFrame_GetFrameIndex, driver/buffer/sync_frame.c) - thay thế hoàn toàn cơ chế đọc
 * SCI_DECODE_TIME + nội suy của VS1053 trước đây.
 * @param
 * @return số sample-frame đã phát, tăng dần đơn điệu cho tới lần reset kế tiếp
 */
uint32_t Max98357a_GetPlayedSamples(void);

/**
 * @brief Max98357a_ResetPlayedSamples
 * Gán lại played_samples về 1 giá trị cụ thể - gọi lúc bắt đầu bài mới (giá trị 0, xem
 * Pcm_Task case BTN_STATE_NEXT/PREV/PLAY_NEW) hoặc lúc seek chính xác tới 1 mốc thời gian bất
 * kỳ (giá trị = targetTimeSec * I2S_SAMPLE_RATE, tính năng seek chưa có UI gọi tới nhưng hàm
 * đã sẵn sàng dùng ngay khi cần).
 * @param value: giá trị played_samples mới
 * @return
 */
void Max98357a_ResetPlayedSamples(uint32_t value);

/**
 * @brief Max98357a_Pause
 * Tắt hẳn clock kênh I2S (i2s_channel_disable()) - im lặng sạch ngay lập tức, KHÁC với việc
 * chỉ đơn thuần ngừng gọi Max98357a_Write() (để nguyên kênh bật sẽ khiến DMA lặp lại khối cuối
 * cùng đã phát, gây tiếng rè/lặp thay vì im lặng). played_samples tự đứng yên theo (không còn
 * callback on_sent nào fire khi kênh đã tắt) - animation tự dừng đúng theo, không cần xử lý gì
 * thêm ở tầng ứng dụng.
 * @param
 * @return ESP_OK nếu tắt thành công, mã lỗi esp_err_t khác nếu thất bại (chỉ log cảnh báo,
 *         không coi là lỗi phần cứng nghiêm trọng - khác Max98357a_Write())
 */
esp_err_t Max98357a_Pause(void);

/**
 * @brief Max98357a_Resume
 * Bật lại clock kênh I2S sau Max98357a_Pause() - PHẢI gọi trước khi Max98357a_Write() tiếp,
 * nếu không lần ghi kế tiếp sẽ lỗi vì kênh đang ở trạng thái disabled.
 * @param
 * @return ESP_OK nếu bật thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Max98357a_Resume(void);

#endif /* MAX98357A_H */
