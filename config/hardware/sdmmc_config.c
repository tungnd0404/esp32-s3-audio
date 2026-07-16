/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "sdmmc_config.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* gSdmmcHost dùng thẳng giá trị mặc định của ESP-IDF (SDMMC_HOST_DEFAULT()) - project không
   cần tinh chỉnh field nào của host (tần số, số bit bus...), nên khởi tạo được ngay tại đây;
   Sdmmc_Init() (driver/sdmmc/sdmmc.c) không cần đụng vào biến này nữa. */
sdmmc_host_t gSdmmcHost = SDMMC_HOST_DEFAULT();

/* gSdmmcSlotConfig KHÔNG thể khởi tạo đầy đủ ngay tại đây như gSdmmcHost - macro
   SDMMC_SLOT_CONFIG_DEFAULT() của ESP-IDF set sẵn nhiều field khác (vd gpio_cd/gpio_wp =
   "không dùng chân CD/WP", KHÔNG phải 0 - nếu bỏ qua macro và tự liệt kê designated
   initializer riêng, các field đó sẽ bị zero-init sai ý nghĩa, dễ hiểu nhầm là "dùng GPIO0
   làm chân CD/WP"). Cần vừa áp macro mặc định vừa ghi đè riêng width/clk/cmd/d0/d1/d2/d3
   theo board - việc này đòi hỏi các câu lệnh gán tuần tự (không gói gọn được trong 1 global
   initializer), nên Sdmmc_Init() (driver/sdmmc/sdmmc.c) vẫn đảm nhiệm xây dựng giá trị cho
   biến này lúc chạy, gọi 1 lần từ app_main() TRƯỚC KHI Sdmmc_Mount() dùng tới. Global storage
   tự động zero-init nên an toàn trong khoảng thời gian trước khi Sdmmc_Init() chạy (không ai
   đọc field nào của biến này trước đó). */
sdmmc_slot_config_t gSdmmcSlotConfig;
