// vs1053.h
#ifndef VS1053_H
#define VS1053_H

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

// Định nghĩa các chân kết nối
#define VS1053_CS_PIN     5   // Chip select (XCS)
#define VS1053_DCS_PIN    26  // Data chip select (XDCS)
#define VS1053_DREQ_PIN   27  // Data request
#define VS1053_RESET_PIN  32  // Reset

// Các thanh ghi SCI
#define SCI_MODE        0x00
#define SCI_STATUS      0x01
#define SCI_BASS        0x02
#define SCI_CLOCKF      0x03
#define SCI_DECODE_TIME 0x04
#define SCI_AUDATA      0x05
#define SCI_WRAM        0x06
#define SCI_WRAMADDR    0x07
#define SCI_HDAT0       0x08
#define SCI_HDAT1       0x09
#define SCI_AIADDR      0x0A
#define SCI_VOL         0x0B
#define SCI_AICTRL0     0x0C
#define SCI_AICTRL1     0x0D
#define SCI_AICTRL2     0x0E
#define SCI_AICTRL3     0x0F

// Các bit trong SCI_MODE
#define SM_SDINEW       (1 << 11)
#define SM_RESET        (1 << 2)
#define SM_CANCEL       (1 << 3)
#define SM_TESTS        (1 << 5)
#define SM_LINE1        (1 << 14)

// Kích thước buffer gửi dữ liệu (32 bytes)
#define VS1053_CHUNK_SIZE 32

// Cấu trúc lưu thông tin thiết bị
typedef struct {
    spi_device_handle_t spi_handle;
    gpio_num_t cs_pin;
    gpio_num_t dcs_pin;
    gpio_num_t dreq_pin;
    gpio_num_t reset_pin;
    uint8_t volume;
    uint8_t end_fill_byte;
    uint8_t chip_version;
} vs1053_handle_t;

// Hàm khởi tạo
esp_err_t vs1053_init(vs1053_handle_t *dev);
esp_err_t vs1053_reset(vs1053_handle_t *dev);
esp_err_t vs1053_soft_reset(vs1053_handle_t *dev);

// Các hàm giao tiếp cơ bản
void vs1053_wait_dreq(vs1053_handle_t *dev);
esp_err_t vs1053_write_sci(vs1053_handle_t *dev, uint8_t reg, uint16_t value);
uint16_t vs1053_read_sci(vs1053_handle_t *dev, uint8_t reg);
esp_err_t vs1053_send_buffer(vs1053_handle_t *dev, const uint8_t *data, size_t len);

// Các hàm điều khiển
esp_err_t vs1053_set_volume(vs1053_handle_t *dev, uint8_t volume);
esp_err_t vs1053_set_tone(vs1053_handle_t *dev, uint16_t tone);
esp_err_t vs1053_switch_to_mp3_mode(vs1053_handle_t *dev);
void vs1053_start_song(vs1053_handle_t *dev);
void vs1053_stop_song(vs1053_handle_t *dev);
void vs1053_cancel_song(vs1053_handle_t *dev);
uint16_t vs1053_get_decoded_time(vs1053_handle_t *dev);
void vs1053_clear_decoded_time(vs1053_handle_t *dev);

// Hàm kiểm tra kết nối
bool vs1053_test_comm(vs1053_handle_t *dev);
void vs1053_print_details(vs1053_handle_t *dev);

#endif // VS1053_H