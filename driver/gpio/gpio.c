/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "gpio.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

esp_err_t Gpio_Init(void)
{
    /* RESET của VS1053 - output, kéo tay bằng gpio_set_level() trong vs1053.c (vs1053_reset()).
       XCS/XDCS (VS1053_XCS_PIN/VS1053_XDCS_PIN) KHÔNG còn cấu hình ở đây nữa - vs1053.c nay
       dùng SPI Master driver chuẩn (spics_io_num), driver tự cấu hình 2 chân đó lúc
       spi_bus_add_device() (xem vs1053_add_spi_devices() trong vs1053.c); cấu hình lại ở đây sẽ chỉ
       là dead config bị driver SPI ghi đè, dễ gây hiểu lầm là module này còn sở hữu 2 chân CS */
    esp_err_t lRet = Gpio_ConfigOutput(1ULL << VS1053_RESET_PIN);
    if (lRet != ESP_OK)
    {
        return lRet;
    }

    /* DREQ của VS1053 - input, Mp3_Task đọc qua gpio_get_level() (xem vs1053_wait_dreq()) */
    return Gpio_ConfigInput(1ULL << VS1053_DREQ_PIN);
}

esp_err_t Gpio_ConfigOutput(uint64_t pinBitMask)
{
    gpio_config_t lIoConf = {
        .pin_bit_mask = pinBitMask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    return gpio_config(&lIoConf);
}

esp_err_t Gpio_ConfigInput(uint64_t pinBitMask)
{
    gpio_config_t lIoConf = {
        .pin_bit_mask = pinBitMask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    return gpio_config(&lIoConf);
}
