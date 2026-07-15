/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <string.h>
#include "vs1053.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Tốc độ SPI lúc mới reset/khởi tạo - chậm, an toàn vì lúc này VS1053 vẫn đang chạy clock
   nội bộ mặc định (chưa nhân clock qua SCI_CLOCKF), SPI nhanh hơn khả năng chip xử lý sẽ mất
   dữ liệu */
#define VS1053_SPI_INIT_CLOCK_HZ    200000U

/* Tốc độ SPI lúc chạy bình thường (sau khi đã cấu hình SCI_CLOCKF = 0x6000, nhân clock lên
   3.0x = 12.288MHz) - nâng lên ở cuối vs1053_init() */
#define VS1053_SPI_RUN_CLOCK_HZ     4000000U

/* Thời gian giữ mức reset / chờ ổn định sau reset (ms) - theo trình tự reset khuyến nghị
   trong datasheet VS1053b */
#define VS1053_RESET_DELAY_MS       100U

/* Số byte "end fill byte" cần gửi để xả hết dữ liệu còn dở trong FIFO nội bộ chip lúc dừng
   bài - con số chuẩn theo tài liệu VS1053 (kích thước FIFO decode lớn nhất có thể gặp) */
#define VS1053_STOP_FILLER_BYTES    2052U

/* Địa chỉ RAM nội bộ chip lưu "end fill byte" đúng cho codec/patch đang nạp - đọc gián tiếp
   qua SCI_WRAMADDR + SCI_WRAM (xem vs1053_start_song) */
#define VS1053_WRAM_ADDR_ENDFILLBYTE 0x1E06U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Tag dùng cho ESP_LOGx trong module này */
static const char *TAG = "VS1053";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief delay_ms
 * Delay theo ms, bọc lại vTaskDelay cho gọn tại các lời gọi trong file này.
 * @param u32Ms: thời gian delay (ms)
 * @return
 */
static inline void delay_ms(uint32_t u32Ms)
{
    vTaskDelay(pdMS_TO_TICKS(u32Ms));
}

/**
 * @brief spi_init
 * Add VS1053 làm 1 SPI device trên SPI_HOST_ID (xem spi.h), tốc độ ban đầu
 * VS1053_SPI_INIT_CLOCK_HZ (an toàn cho lúc chip chưa nhân clock). KHÔNG tự khởi tạo bus -
 * Spi_Init() (spi.c) PHẢI được gọi thành công từ trước (app_main(), trước khi tạo Mp3_Task)
 * vì bus là hạ tầng dùng chung, không thuộc riêng VS1053 (xem lý do tách trong spi.h - sau
 * này device SPI khác chỉ cần tự add lên cùng bus này, không cần khởi tạo lại). CHỈ được gọi
 * đúng 1 lần từ vs1053_init().
 * @param pDev: con trỏ device VS1053, ghi kết quả handle vào pDev->spi_handle nếu thành công
 * @return ESP_OK nếu add device thành công, mã lỗi esp_err_t khác nếu thất bại (vd bus chưa
 *         được Spi_Init())
 */
static esp_err_t spi_init(vs1053_handle_t *pDev)
{
    spi_device_interface_config_t lDevCfg = {
        .clock_speed_hz = VS1053_SPI_INIT_CLOCK_HZ,
        .mode = 0,                         /* SPI mode 0 */
        .spics_io_num = -1,                /* Không dùng CS mặc định của driver SPI - tự kéo tay */
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };

    return spi_bus_add_device(SPI_HOST_ID, &lDevCfg, &pDev->spi_handle);
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

void vs1053_wait_dreq(vs1053_handle_t *pDev)
{
    while (gpio_get_level(pDev->dreq_pin) == 0)
    {
        delay_ms(1);
    }
}

esp_err_t vs1053_write_sci(vs1053_handle_t *pDev, uint8_t reg, uint16_t value)
{
    /* Lệnh ghi SCI: byte 0 = 0x02 (write), byte 1 = địa chỉ thanh ghi, byte 2-3 = dữ liệu
       16-bit (big-endian) */
    spi_transaction_t lTrans = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8 * 4,
        .tx_data = {0x02, reg, (value >> 8) & 0xFF, value & 0xFF}
    };

    gpio_set_level(pDev->cs_pin, 0);
    esp_err_t lRet = spi_device_transmit(pDev->spi_handle, &lTrans);
    gpio_set_level(pDev->cs_pin, 1);

    if (lRet != ESP_OK)
    {
        ESP_LOGW(TAG, "SCI write failed for reg 0x%02X", reg);
    }

    /* Chờ DREQ ngay sau khi gửi xong, để lệnh SCI/SDI KẾ TIẾP (của bất kỳ hàm vs1053_* nào
       gọi sau, không nhất thiết cùng 1 lần gọi) luôn thấy chip đã sẵn sàng - tương đương
       "chờ trước khi gửi" xét trên toàn bộ chuỗi lệnh, chỉ khác vị trí đặt lệnh chờ */
    vs1053_wait_dreq(pDev);
    return lRet;
}

uint16_t vs1053_read_sci(vs1053_handle_t *pDev, uint8_t reg)
{
    uint8_t lau8TxData[4] = {0x03, reg, 0xFF, 0xFF};
    uint8_t lau8RxData[4] = {0};

    spi_transaction_t lTrans = {
        .length = 8 * 4,
        .tx_buffer = lau8TxData,
        .rx_buffer = lau8RxData
    };

    gpio_set_level(pDev->cs_pin, 0);
    esp_err_t lRet = spi_device_transmit(pDev->spi_handle, &lTrans);
    gpio_set_level(pDev->cs_pin, 1);

    if (lRet != ESP_OK)
    {
        /* Không có cách trả lỗi qua giá trị trả về (uint16_t, không phải esp_err_t) - chỉ
           log cảnh báo, bên gọi sẽ nhận nội dung lau8RxData hiện có (rác hoặc 0) */
        ESP_LOGW(TAG, "SCI read failed for reg 0x%02X", reg);
    }

    vs1053_wait_dreq(pDev);
    return (lau8RxData[2] << 8) | lau8RxData[3];
}

esp_err_t vs1053_send_buffer(vs1053_handle_t *pDev, const uint8_t *pData, size_t len)
{
    if (len > VS1053_CHUNK_SIZE)
    {
        ESP_LOGW(TAG, "Buffer size exceeds chunk limit");
        len = VS1053_CHUNK_SIZE;
    }

    /* Chờ DREQ TRƯỚC khi gửi dữ liệu - đảm bảo FIFO nội bộ chip còn chỗ trống, khác với
       vs1053_write_sci()/vs1053_read_sci() (chờ sau) vì đây là đường dữ liệu (SDI, chân DCS)
       tách biệt hoàn toàn khỏi đường lệnh (SCI, chân CS) */
    vs1053_wait_dreq(pDev);

    spi_transaction_t lTrans = {
        .length = 8 * len,
        .tx_buffer = pData,
        .rx_buffer = NULL
    };

    gpio_set_level(pDev->dcs_pin, 0);
    esp_err_t lRet = spi_device_transmit(pDev->spi_handle, &lTrans);
    gpio_set_level(pDev->dcs_pin, 1);

    return lRet;
}

esp_err_t vs1053_reset(vs1053_handle_t *pDev)
{
    /* Kéo RESET xuống thấp, CS/DCS lên cao (nhàn rỗi) trong lúc reset, đợi ổn định, rồi thả
       RESET lên cao và đợi thêm - đúng trình tự reset phần cứng theo datasheet VS1053b */
    gpio_set_level(pDev->reset_pin, 0);
    gpio_set_level(pDev->cs_pin, 1);
    gpio_set_level(pDev->dcs_pin, 1);
    delay_ms(VS1053_RESET_DELAY_MS);
    gpio_set_level(pDev->reset_pin, 1);
    delay_ms(VS1053_RESET_DELAY_MS);

    /* Sau khi thả RESET, chờ DREQ lên cao mới coi là chip đã sẵn sàng nhận lệnh SCI đầu
       tiên (chưa add VS1053 làm SPI device lúc này - vs1053_init() gọi spi_init() ngay sau
       hàm này để add device, bus SPI_HOST_ID thì đã có sẵn từ trước, xem Spi_Init()) */
    vs1053_wait_dreq(pDev);
    return ESP_OK;
}

esp_err_t vs1053_init(vs1053_handle_t *pDev)
{
    /* Cấu hình GPIO: cs/dcs/reset là output, dreq là input - PHẢI làm trước vs1053_reset()
       vì hàm đó cần gpio_set_level() hoạt động đúng trên các chân này */
    gpio_config_t lIoConf = {
        .pin_bit_mask = (1ULL << pDev->cs_pin) | (1ULL << pDev->dcs_pin) | (1ULL << pDev->reset_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&lIoConf);

    lIoConf.pin_bit_mask = (1ULL << pDev->dreq_pin);
    lIoConf.mode = GPIO_MODE_INPUT;
    gpio_config(&lIoConf);

    /* Reset phần cứng trước khi đụng gì tới SPI - chip cần ở trạng thái sạch mới giao tiếp
       đáng tin cậy */
    vs1053_reset(pDev);

    /* Add VS1053 làm SPI device trên bus đã có sẵn (tốc độ thấp, an toàn) - xem spi_init() */
    esp_err_t lRet = spi_init(pDev);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_init failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    /* Kiểm tra kết nối - phát hiện sớm trường hợp chip không có/đấu sai dây trước khi cấu
       hình thêm gì khác */
    if (vs1053_test_comm(pDev) == E_NOT_OK)
    {
        ESP_LOGE(TAG, "VS1053 communication test failed");
        return ESP_FAIL;
    }

    /* Cấu hình clock (3.0x = 12.288 MHz) - PHẢI làm trước khi nâng tốc độ SPI bên dưới,
       nếu không SPI sẽ chạy nhanh hơn khả năng xử lý thật của chip */
    vs1053_write_sci(pDev, SCI_CLOCKF, 0x6000);

    /* Cấu hình lấy mẫu 44.1kHz, STEREO (bit thấp nhất = 0). LƯU Ý: theo datasheet VS1053b,
       bit 0 của SCI_AUDATA là cờ mono/stereo (1 = ép mono), KHÔNG phải 1 phần của sample
       rate - dùng đúng 44100 (số chẵn), không phải 44101, để không vô tình ép chip sang
       mono */
    vs1053_write_sci(pDev, SCI_AUDATA, 44100);

    /* Cấu hình chế độ: SDINEW (bắt buộc) + LINE1 (nguồn vào từ input line thay vì mic) */
    vs1053_write_sci(pDev, SCI_MODE, SM_SDINEW | SM_LINE1);

    /* Thiết lập volume mặc định (50%) */
    vs1053_set_volume(pDev, 50);

    /* Đã cấu hình clock xong - nâng tốc độ SPI lên mức chạy bình thường để tăng thông
       lượng gửi dữ liệu MP3. Phải remove device cũ (tốc độ thấp) rồi add lại với config mới,
       ESP-IDF không cho đổi tốc độ 1 device đang tồn tại */
    spi_device_interface_config_t lDevCfg = {
        .clock_speed_hz = VS1053_SPI_RUN_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };

    esp_err_t lRemoveRet = spi_bus_remove_device(pDev->spi_handle);
    if (lRemoveRet != ESP_OK)
    {
        /* Hiếm khi xảy ra với 1 handle vừa mới add thành công ở trên - chỉ log, không coi
           là lỗi chặn init, vì bước add_device kế tiếp mới quyết định pDev->spi_handle có
           dùng được hay không */
        ESP_LOGW(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(lRemoveRet));
    }

    esp_err_t lAddRet = spi_bus_add_device(SPI_HOST_ID, &lDevCfg, &pDev->spi_handle);
    if (lAddRet != ESP_OK)
    {
        /* Không nâng được tốc độ SPI -> pDev->spi_handle không còn hợp lệ để dùng tiếp,
           coi như init thất bại thay vì âm thầm chạy với handle hỏng */
        ESP_LOGE(TAG, "Failed to raise SPI clock speed after init: %s", esp_err_to_name(lAddRet));
        return lAddRet;
    }

    ESP_LOGI(TAG, "VS1053 initialized successfully");
    return ESP_OK;
}

esp_err_t vs1053_soft_reset(vs1053_handle_t *pDev)
{
    uint16_t lu16Mode = vs1053_read_sci(pDev, SCI_MODE);
    lu16Mode |= SM_RESET;
    esp_err_t lRet = vs1053_write_sci(pDev, SCI_MODE, lu16Mode);

    /* SM_RESET làm chip trả SCI_CLOCKF về mặc định (không nhân clock) - ghi lại ngay tại
       đây để hàm AN TOÀN khi gọi độc lập (không phụ thuộc bên gọi có nhớ khôi phục CLOCKF
       hay không), nếu không SPI bus đang chạy nhanh hơn clock nội bộ mặc định có thể giao
       tiếp sai ở lệnh kế tiếp */
    vs1053_write_sci(pDev, SCI_CLOCKF, 0x6000);

    return lRet;
}

esp_err_t vs1053_set_volume(vs1053_handle_t *pDev, uint8_t volume)
{
    if (volume > 100)
    {
        volume = 100;
    }
    pDev->volume = volume;

    /* SCI_VOL: giá trị càng NHỎ càng to (0x00 = to nhất, 0xFE = nhỏ nhất theo từng nấc),
       nên phải đảo chiều volume (0-100, 100 = to nhất) trước khi ghi thanh ghi */
    uint16_t lu16Vol = ((100 - volume) * 0xFEU / 100U);
    uint16_t lu16Val = (lu16Vol << 8) | lu16Vol;
    return vs1053_write_sci(pDev, SCI_VOL, lu16Val);
}

esp_err_t vs1053_set_tone(vs1053_handle_t *pDev, uint16_t tone)
{
    return vs1053_write_sci(pDev, SCI_BASS, tone);
}

esp_err_t vs1053_switch_to_mp3_mode(vs1053_handle_t *pDev)
{
    /* vs1053_soft_reset() đã tự khôi phục SCI_CLOCKF bên trong - không cần ghi lại ở đây */
    esp_err_t lRet = vs1053_soft_reset(pDev);
    vs1053_write_sci(pDev, SCI_MODE, SM_SDINEW | SM_LINE1);
    vs1053_set_volume(pDev, pDev->volume);
    return lRet;
}

void vs1053_start_song(vs1053_handle_t *pDev)
{
    /* Đọc "end fill byte" thật từ RAM nội bộ chip (địa chỉ VS1053_WRAM_ADDR_ENDFILLBYTE) -
       giá trị này phụ thuộc codec/patch đang nạp, không phải lúc nào cũng là 0. Đọc gián
       tiếp qua SCI_WRAMADDR (chọn địa chỉ) rồi SCI_WRAM (đọc dữ liệu tại địa chỉ đó), theo
       đúng thủ tục truy cập RAM nội bộ của VS1053. Dùng đúng byte này khi flush buffer lúc
       vs1053_stop_song() để tránh tiếng click nhỏ so với hardcode 0 */
    vs1053_write_sci(pDev, SCI_WRAMADDR, VS1053_WRAM_ADDR_ENDFILLBYTE);
    pDev->end_fill_byte = (uint8_t)(vs1053_read_sci(pDev, SCI_WRAM) & 0xFF);
}

void vs1053_stop_song(vs1053_handle_t *pDev)
{
    /* Gửi VS1053_STOP_FILLER_BYTES byte "im lặng" (end_fill_byte) để xả hết dữ liệu còn dở
       trong FIFO nội bộ chip - xem giới hạn đã biết (không SM_CANCEL, không poll) trong doc
       comment ở vs1053.h */
    uint8_t lau8Filler[VS1053_CHUNK_SIZE];
    memset(lau8Filler, pDev->end_fill_byte, VS1053_CHUNK_SIZE);

    for (uint32_t lu32Sent = 0; lu32Sent < VS1053_STOP_FILLER_BYTES; lu32Sent += VS1053_CHUNK_SIZE)
    {
        vs1053_send_buffer(pDev, lau8Filler, VS1053_CHUNK_SIZE);
    }

    delay_ms(VS1053_RESET_DELAY_MS);
}

void vs1053_cancel_song(vs1053_handle_t *pDev)
{
    uint16_t lu16Mode = vs1053_read_sci(pDev, SCI_MODE);
    lu16Mode |= SM_CANCEL;
    vs1053_write_sci(pDev, SCI_MODE, lu16Mode);
}

uint16_t vs1053_get_decoded_time(vs1053_handle_t *pDev)
{
    return vs1053_read_sci(pDev, SCI_DECODE_TIME);
}

void vs1053_clear_decoded_time(vs1053_handle_t *pDev)
{
    vs1053_write_sci(pDev, SCI_DECODE_TIME, 0);
}

Std_ReturnType vs1053_test_comm(vs1053_handle_t *pDev)
{
    if (gpio_get_level(pDev->dreq_pin) == 0)
    {
        ESP_LOGE(TAG, "VS1053 not detected (DREQ low)");
        return E_NOT_OK;
    }

    /* Ghi 1 giá trị test bất kỳ vào SCI_VOL rồi đọc lại - đọc đúng giá trị vừa ghi nghĩa là
       kênh SPI hoạt động đúng cả 2 chiều. Luôn khôi phục lại giá trị cũ ngay sau đó, không
       ảnh hưởng volume thật đang phát */
    uint16_t lu16OldVol = vs1053_read_sci(pDev, SCI_VOL);
    vs1053_write_sci(pDev, SCI_VOL, 0x1234);
    uint16_t lu16Test = vs1053_read_sci(pDev, SCI_VOL);
    vs1053_write_sci(pDev, SCI_VOL, lu16OldVol);

    return ((lu16Test == 0x1234) ? E_OK : E_NOT_OK);
}

void vs1053_print_details(vs1053_handle_t *pDev)
{
    ESP_LOGI(TAG, "VS1053 Details:");
    ESP_LOGI(TAG, "  Mode:    0x%04X", vs1053_read_sci(pDev, SCI_MODE));
    ESP_LOGI(TAG, "  Status:  0x%04X", vs1053_read_sci(pDev, SCI_STATUS));
    ESP_LOGI(TAG, "  ClockF:  0x%04X", vs1053_read_sci(pDev, SCI_CLOCKF));
    ESP_LOGI(TAG, "  Volume:  0x%04X", vs1053_read_sci(pDev, SCI_VOL));
    ESP_LOGI(TAG, "  Audio:   0x%04X", vs1053_read_sci(pDev, SCI_AUDATA));
    ESP_LOGI(TAG, "  HDAT0:   0x%04X", vs1053_read_sci(pDev, SCI_HDAT0));
    ESP_LOGI(TAG, "  HDAT1:   0x%04X", vs1053_read_sci(pDev, SCI_HDAT1));
}
