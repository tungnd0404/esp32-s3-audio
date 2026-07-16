/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "button_config.h"

/* button_config.h hiện chỉ có #define (chân GPIO) nên không cần cấp phát biến gì ở đây -
   file này để trống, dự phòng cho sau này nếu cần thêm biến cấu hình runtime riêng cho
   button (vd debounce time có thể tinh chỉnh lúc chạy thay vì hardcode trong button.c).
   Cấu hình gpio_config_t dùng chung cho nút bấm (gButtonGpioConfig) nằm ở gpio_config.c/.h,
   không phải ở đây, vì đó là kiểu dữ liệu chung của GPIO (driver/gpio.h), không riêng gì
   button - xem gpio_config.h */
