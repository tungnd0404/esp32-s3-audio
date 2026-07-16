/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "gpio_config.h"
#include "button_config.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) cấu hình GPIO dùng chung cho toàn bộ nút bấm - khai báo extern
   trong gpio_config.h. pin_bit_mask gộp cả 3 chân hiện có (button_config.h); thêm nút bấm
   mới chỉ cần OR thêm (1ULL << BUTTON_xxx_PIN) vào đây sau khi đã thêm macro chân tương ứng
   trong button_config.h. */
gpio_config_t gButtonGpioConfig =
{
    .intr_type = GPIO_INTR_NEGEDGE,
    .mode = GPIO_MODE_INPUT,
    .pin_bit_mask =
        (1ULL << BUTTON_NEXT_PIN) |
        (1ULL << BUTTON_PREV_PIN) |
        (1ULL << BUTTON_PLAY_PIN),
    .pull_up_en = GPIO_PULLUP_ENABLE,
};
