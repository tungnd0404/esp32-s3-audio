#include "config.h"
#include "sync_frame.h"
#include "esp_timer.h"
// Biến toàn cục
static unsigned long lastSync = 0;          // mốc 100ms cuối
static uint16_t decodePrev = 0;              // giá trị decode_time lần trước
static uint16_t decodeTotal = 0;
static volatile float virtualTime   = 0.0f;           // thời gian phát ảo (mượt)

/* ==============================
   Khởi tạo đồng bộ (gọi 1 lần)
=============================== */
void initSync() {
    lastSync = esp_timer_get_time() / 1000;  // ms();
    decodePrev = /* VS1053_ReadRegister(SCI_DECODE_TIME) */ 0u;
    decodeTotal = decodePrev;
    virtualTime = (float)decodePrev;
}

/* ==============================
   Cập nhật thời gian ảo (gọi mỗi vòng lặp)
=============================== */
static int get_virtualtime() {
    unsigned long now = esp_timer_get_time() / 1000;  // ms();

    // Xử lý các mốc 100ms bị bỏ lỡ (chống trượt)
    while (now - lastSync >= 100) {
        lastSync += 100;

        // Đọc thanh ghi decode_time (16-bit, tăng mỗi giây, có thể rollover)
        uint16_t t_raw = /* VS1053_ReadRegister(SCI_DECODE_TIME) */ 0u;

        // Tính delta (có xử lý rollover)
        uint16_t delta_raw;
        if (t_raw >= decodePrev) {
            delta_raw = t_raw - decodePrev;
        } else {
            // Rollover: 65535 -> 0, khoảng cách = (65535 - decodePrev) + t_raw + 1
            delta_raw = (65535 - decodePrev) + t_raw + 1;
        }

        // Giới hạn delta tối đa 1 giây (chống nhảy do nhiễu)
        if (delta_raw > 1) delta_raw = 1;

        // Cập nhật decodePrev (RAW) và decodeTotal (tuyệt đối)
        decodePrev = t_raw;
        decodeTotal += delta_raw;

        // Nếu có sự thay đổi giây (delta_raw > 0) -> reset phần lẻ nội suy
        // Nhưng vì virtualTime sẽ được tính lại ở cuối hàm nên không cần xử lý thêm
    }

    // Nội suy thời gian ảo trong khoảng 100ms hiện tại
    float delta_ms = (float)(now - lastSync) / 1000.0f;
    virtualTime = (float)decodeTotal + delta_ms;
    return virtualTime;
}

/**
 * Tính chỉ số frame cần lấy tại thời điểm virtualTime
 * @param virtualTime: thời gian phát ảo (đơn vị giây, từ bộ đồng bộ MP3)
 * @return Chỉ số frame (bắt đầu từ 0)
 */
uint32_t syncFrameWithMp3() {

    float virtualTime = get_virtualtime();   // hoặc đọc từ biến virtualTime
    // Đảm bảo virtualTime không âm (phòng trường hợp lỗi)
    if (virtualTime < 0) virtualTime = 0;

    // Tính frame index dựa trên số giây * fps
    uint32_t frameIndex = (uint32_t)(virtualTime * FRAME_PER_SECOND);

    return frameIndex;
} 

/* ==============================
   Task đồng bộ (chạy song song)
=============================== */
/* void syncTask(void *param) {
    void initSync();
    for (;;) {
        get_virtualtime();                        // cập nhật virtualTime
        vTaskDelay(50 / portTICK_PERIOD_MS); // gọi mỗi 50ms
    }
} */