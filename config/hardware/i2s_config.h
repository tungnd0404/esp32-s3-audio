#ifndef I2S_CONFIG_H
#define I2S_CONFIG_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* --- Chân I2S vật lý dùng cho MAX98357A - đổi tại đây nếu đấu dây khác board. Tái dùng đúng
   3 chân MOSI/MISO/SCLK cũ của SPI2 (VS1053, đã gỡ bỏ) vì đã có sẵn dây tại đó. MAX98357A
   KHÔNG cần MCLK (đã xác nhận qua datasheet Analog Devices + tài liệu Adafruit - chip tự suy
   ra clock từ BCLK/WS), nên chỉ cần đúng 3 chân BCLK/WS/DOUT, không có MISO/DIN vì đây là
   kênh phát (TX) thuần, không thu âm --- */
#define I2S_BCLK_PIN     GPIO_NUM_13
#define I2S_WS_PIN       GPIO_NUM_12
#define I2S_DOUT_PIN     GPIO_NUM_11

/* Chân SD (Shutdown/enable) của MAX98357A - GPIO_NUM_NC nghĩa là KHÔNG điều khiển qua phần
   mềm, đấu cứng chân SD lên VDD qua board (loa luôn bật). Nếu sau này cần tắt loa hẳn qua
   phần mềm (tiết kiệm điện lúc Pause dài), đổi macro này thành 1 GPIO thật rồi tự thêm
   gpio_set_level() vào Max98357a_Pause()/Max98357a_Resume() (driver/max98357a/max98357a.c) -
   hiện tại Pause chỉ tắt clock I2S (i2s_channel_disable()), không tắt hẳn nguồn ampli */
#define I2S_SD_PIN       GPIO_NUM_NC

/* --- Định dạng PCM - PHẢI khớp tuyệt đối với tool chuyển đổi MP3 -> PCM chạy trên PC, vì
   file .pcm trên thẻ SD không có header tự mô tả định dạng (khác .wav). Đổi 1 trong 2 nơi mà
   không đổi nơi còn lại sẽ làm nhạc phát sai tốc độ/rè (lệch sample rate) hoặc lệch kênh
   trái-phải (lệch bit depth/số kênh) --- */
#define I2S_SAMPLE_RATE       44100U
#define I2S_BITS_PER_SAMPLE   16U
#define I2S_CHANNEL_COUNT     2U

/* Độ sâu đệm DMA của kênh I2S TX - quyết định độ trễ audio VÀ khả năng chống underrun khi
   Sdcard_Task/thẻ SD chậm nhất thời. Độ trễ ước tính = I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM /
   I2S_SAMPLE_RATE giây = 6*240/44100 ~= 32.7ms - đủ thấp để animation (đồng bộ theo
   played_samples, xem driver/buffer/sync_frame.c) không lệch cảm nhận được, vẫn đủ sâu để
   chống giật khi đọc thẻ SD chậm thoáng qua */
#define I2S_DMA_DESC_NUM      6U
#define I2S_DMA_FRAME_NUM     240U

#endif /* I2S_CONFIG_H */
