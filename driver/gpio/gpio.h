#ifndef GPIO_H
#define GPIO_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/gpio.h"
#include "esp_err.h"
#include "vs1053_config.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Gpio_Init
 * Cấu hình TOÀN BỘ chân GPIO thuần (không qua bus SPI/I2C) mà hệ thống cần, ngay từ app_main(),
 * TRƯỚC KHI bất kỳ task nào được tạo - cùng nguyên tắc với Spi_Init()/I2c_Init(): mọi ngoại vi
 * dùng chung phải sẵn sàng trước khi task bắt đầu chạy, task không tự cấu hình phần cứng dùng
 * chung của chính nó nữa. Hiện chỉ có 2 chân DREQ/RESET của VS1053 (xem vs1053_config.h) -
 * XCS/XDCS KHÔNG nằm ở đây vì do driver SPI Master tự cấu hình qua spics_io_num (xem
 * vs1053_add_spi_devices() trong vs1053.c). Nếu sau này thêm ngoại vi khác cần GPIO thuần, thêm lời gọi
 * Gpio_ConfigOutput()/ConfigInput() tương ứng vào đây, KHÔNG tự gpio_config() rải rác ở module
 * khác.
 * @param
 * @return ESP_OK nếu cấu hình thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Gpio_Init(void);

/**
 * @brief Gpio_ConfigOutput
 * Cấu hình 1 hoặc nhiều chân GPIO làm output (pull-up bật, pull-down tắt, không dùng ngắt) -
 * dùng chung cho mọi driver cần chân điều khiển thuần digital (vd RESET của VS1053, xem
 * vs1053_reset() trong vs1053.c), tránh mỗi driver tự khai báo lại 1 gpio_config_t giống hệt
 * nhau. Không tự biết ý nghĩa từng chân (RESET/...) - bên gọi tự ghép pin_bit_mask.
 * @param pinBitMask: bitmask các chân cần cấu hình (vd (1ULL << pin1) | (1ULL << pin2))
 * @return ESP_OK nếu cấu hình thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Gpio_ConfigOutput(uint64_t pinBitMask);

/**
 * @brief Gpio_ConfigInput
 * Cấu hình 1 hoặc nhiều chân GPIO làm input (pull-up bật, pull-down tắt, không dùng ngắt) -
 * cùng khuôn mẫu Gpio_ConfigOutput(), dùng cho các chân thuần đọc trạng thái (vd DREQ của
 * VS1053).
 * @param pinBitMask: bitmask các chân cần cấu hình (vd (1ULL << pin))
 * @return ESP_OK nếu cấu hình thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t Gpio_ConfigInput(uint64_t pinBitMask);

#endif /* GPIO_H */
