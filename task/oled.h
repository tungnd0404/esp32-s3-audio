#ifndef OLED_H
#define OLED_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "ssd1306.h"
#include "std_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
extern TaskHandle_t xOledTaskHandle;

/* Hàng đợi lệnh dùng chung tới Oled_Task (phần tử kiểu Srm_Message_s), do Oled_Init() tạo
   (Oled_Task là owner duy nhất của màn hình SSD1306 - kiến trúc Owner Task, xem srm.h).
   Hiện CHƯA có lệnh nào (Srm_CommandType_e) thật sự dùng tới hạ tầng này, vì chưa có module
   nào khác cần đụng vào SSD1306 (khác với VS1053/double buffer, vốn được nhiều task chia sẻ)
   - dựng sẵn hạ tầng owner (queue + Oled_HandleCommand + Oled_ServicePendingCommand trong
   oled.c) để nếu sau này có nhu cầu, chỉ cần thêm 1 case OLED_CMD_* + 1 hàm Srm_Oled<Lệnh>()
   gửi, không phải sửa lại cấu trúc Oled_Task */
extern QueueHandle_t xOledCommandQueue;

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Trạng thái mount/quét thẻ SD lúc boot, dùng làm giá trị payload cho lệnh
   OLED_CMD_SHOW_STATUS (xem srm.h/Srm_OledNotifyBootStatus) - Sdcard_Task gửi đúng 1 lần
   ngay sau khi quét xong, Oled_Task hiển thị lỗi lên màn hình nếu cần trước khi vẽ menu */
#define OLED_BOOT_STATUS_OK           0U
#define OLED_BOOT_STATUS_SD_ERROR     1U
#define OLED_BOOT_STATUS_NO_SONGS     2U

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_Init
 * Add SSD1306 làm I2C device trên bus có sẵn (YÊU CẦU I2c_Init() - i2c.c - đã gọi thành công
 * từ trước, xem i2c.h) rồi khởi tạo màn hình SSD1306. Gọi bởi chính Oled_Task lúc khởi động
 * (cùng khuôn mẫu Mp3_Task tự gọi vs1053_init() - xem mp3.c), không còn gọi từ app_main.
 * Tự kiểm tra gI2cBusHandle trước khi add device - nếu I2c_Init() (app_main(), main/audio.c)
 * thất bại từ trước (bus chưa từng được tạo, gI2cBusHandle vẫn NULL), trả về E_NOT_OK NGAY,
 * KHÔNG gọi ssd1306_add_i2c_device() - hàm đó dùng ESP_ERROR_CHECK() nội bộ
 * (driver/ssd1306/ssd1306_i2c.c) nên add device lên bus NULL sẽ khiến toàn hệ thống abort/
 * reboot ngay lập tức thay vì chỉ riêng Oled_Task gặp lỗi. Cùng khuôn mẫu "task tự phát hiện
 * lỗi phần cứng của chính mình và tự halt" đã dùng cho vs1053_init() (xem Mp3_Task, mp3.c).
 * @param dev: con trỏ device SSD1306
 * @return E_OK nếu add device + khởi tạo màn hình thành công, E_NOT_OK nếu gI2cBusHandle chưa
 *         sẵn sàng (I2c_Init() thất bại) - xem Oled_Task để biết cách xử lý khi nhận E_NOT_OK
 */
Std_ReturnType Oled_Init(SSD1306_t *dev);

/**
 * @brief Oled_Task
 * Task điều khiển màn hình OLED, nhận PlayerManager_ButtonStateType_e từ PlayerManager_Task
 * qua task notification (dùng chung thẳng enum này, không cần thêm 1 enum sự kiện riêng cho OLED).
 * Tự khai báo device SSD1306 làm biến local và tự gọi Oled_Init() lúc khởi động.
 * @param pvParameters: không dùng, luôn truyền NULL lúc tạo task
 * @return
 */
void Oled_Task(void *pvParameters);

#endif /* OLED_H */
