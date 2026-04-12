#include "syncTask.h"
// Biến toàn cục
unsigned long lastSync = 0;          // mốc 100ms cuối
uint16_t decodePrev = 0;              // giá trị decode_time lần trước
uint16_t decodeNow  = 0;              // giá trị decode_time hiện tại
volatile float virtualTime   = 0.0f;           // thời gian phát ảo (mượt)

/* ==============================
    Sync giữa Mp3 và frame
================================== */
// GỌI MỖI VÒNG (loop hoặc SD task)
void syncTime() {
    unsigned long now = millis();

    // ---- Anti-slip: bắt kịp các mốc 100ms, không bao giờ miss ----
    while (now - lastSync >= 100) {
        lastSync += 100;

        // Đọc thanh ghi decode_time
        uint16_t t = VS1053_ReadRegister(SCI_DECODE_TIME);

        // ----------- LỌC AN TOÀN GIÂY -----------
        // 1. Không cho phép GIẢM (chống tụt giây)
        if (t < decodePrev)
            t = decodePrev;

        // 2. Không cho phép tăng QUÁ 1 giây
        if (t > decodePrev + 1)
            t = decodePrev + 1;

        // Cập nhật
        decodePrev = decodeNow;
        decodeNow  = t;

        // Khi tăng giây → reset drift
        if (decodeNow != decodePrev)
            virtualTime = decodeNow;
    }

    // ----------- Nội suy thời gian ảo -----------
    float delta = (now - lastSync) / 1000.0f;
    virtualTime = decodeNow + delta;
}

/* ================================
        Sync task
    =============================== */
void syncTask(void *param) {
    for (;;) {
        syncTime();          // cập nhật virtualTime
        vTaskDelay(50 / portTICK_PERIOD_MS);  // chạy mỗi 50 ms
    }
}