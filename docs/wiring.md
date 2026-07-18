# Bảng đấu dây phần cứng

Tổng hợp từ `config/hardware/*.h`. Nếu sau này đổi chân trong code, nhớ cập nhật lại file này.

## 1. Nút bấm (`button_config.h`)

| Tín hiệu | GPIO | Nối vào |
|---|---|---|
| BUTTON_NEXT_PIN | GPIO4 | 1 chân nút Next → GPIO4, chân còn lại → GND |
| BUTTON_PREV_PIN | GPIO5 | 1 chân nút Prev → GPIO5, chân còn lại → GND |
| BUTTON_PLAY_PIN | GPIO6 | 1 chân nút Play/Pause → GPIO6, chân còn lại → GND |

Ghi chú: input, kiểm tra `gButtonGpioConfig` (`gpio_config.c`) xem có bật pull-up nội bộ không. Nếu không, thêm điện trở pull-up ngoài 10kΩ lên 3.3V cho mỗi nút.

## 2. Màn hình OLED SSD1306 (I2C) — `i2c_config.h` + `ssd1306_config.h`

| Tín hiệu | GPIO | Nối vào |
|---|---|---|
| I2C0_SDA_PIN | GPIO8 | SDA của module SSD1306 |
| I2C0_SCL_PIN | GPIO9 | SCL của module SSD1306 |
| SSD1306_RESET_PIN | không dùng (NC) | không nối — reset qua phần mềm |
| — | — | VCC module → 3.3V 5v đều được, GND → GND |

Ghi chú: nếu module không có sẵn trở pull-up, thêm 2 trở 4.7kΩ từ SDA/SCL lên 3.3V.

## 3. VS1053 (giải mã MP3, qua SPI2) — `spi_config.h` + `vs1053_config.h`

| Tín hiệu | GPIO | Nối vào chân VS1053 |
|---|---|---|
| SPI2_MOSI_PIN | GPIO11 | SI (MOSI) |
| SPI2_MISO_PIN | GPIO12 | SO (MISO) |
| SPI2_SCLK_PIN | GPIO13 | SCK |
| VS1053_XCS_PIN | GPIO14 | XCS (chip select lệnh điều khiển) |
| VS1053_XDCS_PIN | GPIO15 | XDCS (chip select dữ liệu) |
| VS1053_DREQ_PIN | GPIO16 | DREQ (báo sẵn sàng nhận data) |
| VS1053_RESET_PIN | GPIO17 | RESET (tích cực mức thấp) |
| — | — | VCC → 3.3V hoặc 5V tùy module, GND → GND |

## 4. Thẻ nhớ SD (SDMMC 1-bit) — `sdmmc_config.h`

| Tín hiệu | GPIO | Nối vào |
|---|---|---|
| SDMMC_CLK_PIN | GPIO39 | CLK của khe thẻ SD |
| SDMMC_CMD_PIN | GPIO38 | CMD của khe thẻ SD |
| SDMMC_D0_PIN | GPIO40 | D0 của khe thẻ SD |
| — | — | VCC → 3.3V, GND → GND. D1/D2/D3 không dùng (bus width = 1) |

## Ghi chú chung

- Tất cả module dùng chung 3.3V và GND — nối GND các module về cùng 1 điểm với GND của ESP32-S3.
- Không có chân nào bị trùng giữa các nhóm ở trên (đã rà lại toàn bộ).
- SPI2 (MOSI/MISO/SCLK) hiện chỉ VS1053 dùng — nếu thêm thiết bị SPI khác thì dùng chung 3 chân này, thêm chân CS riêng cho thiết bị mới.
