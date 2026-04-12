// vs1053.c
#include "vs1053.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "VS1053";

// Hàm trễ (ms)
static inline void delay_ms(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Khởi tạo SPI
static esp_err_t spi_init(vs1053_handle_t *dev) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_23,
        .miso_io_num = GPIO_NUM_19,
        .sclk_io_num = GPIO_NUM_18,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = VS1053_CHUNK_SIZE
    };
    
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 200000,          // Tốc độ thấp khi khởi tạo
        .mode = 0,                         // SPI mode 0
        .spics_io_num = -1,                // Không dùng CS mặc định
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;
    
    return spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev->spi_handle);
}

// Hàm đợi DREQ lên mức cao (có thể nhận dữ liệu)
void vs1053_wait_dreq(vs1053_handle_t *dev) {
    while (gpio_get_level(dev->dreq_pin) == 0) {
        delay_ms(1);
    }
}

// Ghi thanh ghi SCI
esp_err_t vs1053_write_sci(vs1053_handle_t *dev, uint8_t reg, uint16_t value) {
    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8 * 4,                    // 4 bytes: command + reg + 2 bytes data
        .tx_data = {0x02, reg, (value >> 8) & 0xFF, value & 0xFF}
    };
    
    gpio_set_level(dev->cs_pin, 0);
    esp_err_t err = spi_device_transmit(dev->spi_handle, &trans);
    gpio_set_level(dev->cs_pin, 1);
    
    vs1053_wait_dreq(dev);
    return err;
}

// Đọc thanh ghi SCI
uint16_t vs1053_read_sci(vs1053_handle_t *dev, uint8_t reg) {
    uint8_t tx_data[4] = {0x03, reg, 0xFF, 0xFF};
    uint8_t rx_data[4] = {0};
    
    spi_transaction_t trans = {
        .length = 8 * 4,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data
    };
    
    gpio_set_level(dev->cs_pin, 0);
    spi_device_transmit(dev->spi_handle, &trans);
    gpio_set_level(dev->cs_pin, 1);
    
    vs1053_wait_dreq(dev);
    return (rx_data[2] << 8) | rx_data[3];
}

// Gửi buffer dữ liệu 32 byte tới VS1053
esp_err_t vs1053_send_buffer(vs1053_handle_t *dev, const uint8_t *data, size_t len) {
    if (len > VS1053_CHUNK_SIZE) {
        ESP_LOGW(TAG, "Buffer size exceeds chunk limit");
        len = VS1053_CHUNK_SIZE;
    }
    
    vs1053_wait_dreq(dev);
    
    spi_transaction_t trans = {
        .length = 8 * len,
        .tx_buffer = data,
        .rx_buffer = NULL
    };
    
    gpio_set_level(dev->dcs_pin, 0);
    esp_err_t err = spi_device_transmit(dev->spi_handle, &trans);
    gpio_set_level(dev->dcs_pin, 1);
    
    return err;
}

// Hàm khởi tạo VS1053
esp_err_t vs1053_init(vs1053_handle_t *dev) {
    // Cấu hình các chân GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << dev->cs_pin) | (1ULL << dev->dcs_pin) | (1ULL << dev->reset_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    io_conf.pin_bit_mask = (1ULL << dev->dreq_pin);
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    
    // Reset VS1053
    gpio_set_level(dev->reset_pin, 0);
    gpio_set_level(dev->cs_pin, 1);
    gpio_set_level(dev->dcs_pin, 1);
    delay_ms(100);
    gpio_set_level(dev->reset_pin, 1);
    delay_ms(100);
    
    // Khởi tạo SPI
    esp_err_t err = spi_init(dev);
    if (err != ESP_OK) return err;
    
    // Chờ DREQ sau reset
    vs1053_wait_dreq(dev);
    
    // Kiểm tra kết nối
    if (!vs1053_test_comm(dev)) {
        ESP_LOGE(TAG, "VS1053 communication test failed");
        return ESP_FAIL;
    }
    
    // Cấu hình clock (3.0x = 12.288 MHz)
    vs1053_write_sci(dev, SCI_CLOCKF, 0x6000);
    
    // Cấu hình âm thanh: 44.1kHz stereo
    vs1053_write_sci(dev, SCI_AUDATA, 44101);
    
    // Cấu hình chế độ: SDINEW + LINE_IN
    vs1053_write_sci(dev, SCI_MODE, SM_SDINEW | SM_LINE1);
    
    // Thiết lập volume mặc định (50%)
    vs1053_set_volume(dev, 50);
    
    // Cập nhật tốc độ SPI lên 4MHz
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
    spi_bus_remove_device(dev->spi_handle);
    spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev->spi_handle);
    
    ESP_LOGI(TAG, "VS1053 initialized successfully");
    return ESP_OK;
}

// Reset mềm VS1053
esp_err_t vs1053_soft_reset(vs1053_handle_t *dev) {
    uint16_t mode = vs1053_read_sci(dev, SCI_MODE);
    mode |= SM_RESET;
    return vs1053_write_sci(dev, SCI_MODE, mode);
}

// Thiết lập volume (0-100)
esp_err_t vs1053_set_volume(vs1053_handle_t *dev, uint8_t volume) {
    if (volume > 100) volume = 100;
    dev->volume = volume;
    uint16_t vol = ((100 - volume) * 0xFE / 100);
    uint16_t val = (vol << 8) | vol;
    return vs1053_write_sci(dev, SCI_VOL, val);
}

// Chuyển sang chế độ MP3
esp_err_t vs1053_switch_to_mp3_mode(vs1053_handle_t *dev) {
    vs1053_soft_reset(dev);
    vs1053_wait_dreq(dev);
    vs1053_write_sci(dev, SCI_CLOCKF, 0x6000);
    vs1053_write_sci(dev, SCI_MODE, SM_SDINEW | SM_LINE1);
    vs1053_set_volume(dev, dev->volume);
    return ESP_OK;
}

// Bắt đầu bài hát mới
void vs1053_start_song(vs1053_handle_t *dev) {
    dev->end_fill_byte = 0;
}

// Dừng bài hát
void vs1053_stop_song(vs1053_handle_t *dev) {
    // Gửi 2052 bytes fillers để đảm bảo buffer được xóa
    for (int i = 0; i < 2052; i += VS1053_CHUNK_SIZE) {
        uint8_t filler[VS1053_CHUNK_SIZE];
        memset(filler, dev->end_fill_byte, VS1053_CHUNK_SIZE);
        vs1053_send_buffer(dev, filler, VS1053_CHUNK_SIZE);
    }
    delay_ms(100);
}

// Hủy bài hát hiện tại
void vs1053_cancel_song(vs1053_handle_t *dev) {
    uint16_t mode = vs1053_read_sci(dev, SCI_MODE);
    mode |= SM_CANCEL;
    vs1053_write_sci(dev, SCI_MODE, mode);
}

// Lấy thời gian giải mã (giây)
uint16_t vs1053_get_decoded_time(vs1053_handle_t *dev) {
    return vs1053_read_sci(dev, SCI_DECODE_TIME);
}

// Xóa thời gian giải mã
void vs1053_clear_decoded_time(vs1053_handle_t *dev) {
    vs1053_write_sci(dev, SCI_DECODE_TIME, 0);
}

// Kiểm tra kết nối với VS1053
bool vs1053_test_comm(vs1053_handle_t *dev) {
    if (gpio_get_level(dev->dreq_pin) == 0) {
        ESP_LOGE(TAG, "VS1053 not detected (DREQ low)");
        return false;
    }
    
    uint16_t old_vol = vs1053_read_sci(dev, SCI_VOL);
    vs1053_write_sci(dev, SCI_VOL, 0x1234);
    uint16_t test = vs1053_read_sci(dev, SCI_VOL);
    vs1053_write_sci(dev, SCI_VOL, old_vol);
    
    return (test == 0x1234);
}

// In thông tin chi tiết
void vs1053_print_details(vs1053_handle_t *dev) {
    ESP_LOGI(TAG, "VS1053 Details:");
    ESP_LOGI(TAG, "  Mode:    0x%04X", vs1053_read_sci(dev, SCI_MODE));
    ESP_LOGI(TAG, "  Status:  0x%04X", vs1053_read_sci(dev, SCI_STATUS));
    ESP_LOGI(TAG, "  ClockF:  0x%04X", vs1053_read_sci(dev, SCI_CLOCKF));
    ESP_LOGI(TAG, "  Volume:  0x%04X", vs1053_read_sci(dev, SCI_VOL));
    ESP_LOGI(TAG, "  Audio:   0x%04X", vs1053_read_sci(dev, SCI_AUDATA));
    ESP_LOGI(TAG, "  HDAT0:   0x%04X", vs1053_read_sci(dev, SCI_HDAT0));
    ESP_LOGI(TAG, "  HDAT1:   0x%04X", vs1053_read_sci(dev, SCI_HDAT1));
}