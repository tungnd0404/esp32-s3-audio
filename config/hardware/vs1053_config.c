/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "vs1053.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Định nghĩa (cấp phát thật) instance DUY NHẤT của vs1053_handle_t - khai báo extern trong
   vs1053.h. Chỉ gán sẵn 2 field cấu hình board tĩnh (dreq_pin/reset_pin, xem
   vs1053_config.h); các field còn lại (sci_handle/sdi_handle/volume/end_fill_byte/
   chip_version) không liệt kê trong designated initializer nên tự động = 0/NULL theo đúng
   chuẩn C (aggregate initialization) - vs1053_init() tự gán lại các field đó lúc chạy
   (vs1053_add_spi_devices() gán sci_handle/sdi_handle, vs1053_set_volume() gán volume,
   vs1053_start_song() gán end_fill_byte). */
vs1053_handle_t gVs1053DeviceInfo = 
{ 
   .dreq_pin = VS1053_DREQ_PIN, 
   .reset_pin = VS1053_RESET_PIN 
};
