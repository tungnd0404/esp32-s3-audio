/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdcard.h"
#include "oled.h"
#include "menu.h"
#include "button.h"
#include "player_manager.h"
#include "srm.h"
#include "mp3.h"
#include "spi.h"
#include "i2c.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Stack size (word... thực ra byte, xTaskCreatePinnedToCore nhận usStackDepth theo byte trên
   ESP-IDF) dùng chung cho cả 4 task - đủ dư cho nhu cầu hiện tại của từng task (FATFS/SPI/I2C
   transaction structs cỡ vài trăm byte, không có buffer lớn nào nằm trên stack - các buffer
   1024/4096 byte của double buffer/ring buffer đều là static/global, xem double_buffer.c/
   ring_buffer.c). Nếu sau này thêm việc gì tốn stack hơn, dùng uxTaskGetStackHighWaterMark()
   để đo thực tế thay vì đoán */
#define AUDIO_TASK_STACK_SIZE   4096U

/* Core chạy Mp3_Task/Sdcard_Task/PlayerManager_Task - nhóm các task có liên quan chặt tới
   luồng audio (Sdcard_Task nạp dữ liệu cho Mp3_Task qua xMp3RingBuffer, PlayerManager_Task
   điều khiển cả 2) chung 1 core, tách khỏi Oled_Task (thuần cosmetic, ít nhạy cảm nhất) */
#define AUDIO_TASK_CORE          1

/* Core chạy Oled_Task riêng - project không dùng WiFi/BT nên Core 0 gần như rảnh, tách
   Oled_Task ra đây để không tranh CPU với Mp3_Task/Sdcard_Task trên Core 1, giảm rủi ro giật/
   rè âm thanh nếu Oled_Task đang vẽ animation (I2C/SPI) đúng lúc Mp3_Task cần chạy */
#define OLED_TASK_CORE           0

/* Priority tương đối giữa 4 task - CAO hơn = ưu tiên CPU hơn khi có tranh chấp (tất cả vẫn
   thấp hơn nhiều so với priority các task hệ thống ESP-IDF, vd WiFi task ~18-23, nên không
   ảnh hưởng tới phần lõi hệ thống dù project hiện chưa dùng WiFi):
   - Mp3_Task CAO NHẤT: nhạy cảm với độ trễ nhất trong 4 task - chậm 1 nhịp là nghe thấy giật/
     rè ngay lập tức, khác với animation/UI (chậm 1 frame gần như không ai nhận ra)
   - Sdcard_Task/PlayerManager_Task ngang Mp3_Task cũ (mức "mặc định" trước đây) - Sdcard_Task
     cần chạy đều để không làm cạn xMp3RingBuffer, PlayerManager_Task cần phản hồi nút bấm
     nhanh, nhưng công việc mỗi lần đều ngắn nên không cần cao bằng Mp3_Task
   - Oled_Task THẤP NHẤT: chấp nhận được nếu bị trễ 1 chút do phải nhường CPU cho các task còn
     lại - animation lệch vài chục ms không ai để ý bằng audio giật */
#define AUDIO_TASK_PRIORITY_MP3      6U
#define AUDIO_TASK_PRIORITY_SDCARD   5U
#define AUDIO_TASK_PRIORITY_PLAYER   5U
#define AUDIO_TASK_PRIORITY_OLED     4U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "AUDIO";

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief app_main
 * Điểm vào chương trình (entry point ESP-IDF): khởi tạo tuần tự từng module (thứ tự PHẢI
 * đúng như dưới đây - xem comment tại từng bước), tạo đủ 4 task chính (Sdcard_Task/Mp3_Task/
 * PlayerManager_Task/Oled_Task) - mount/quét thẻ SD và khởi tạo màn hình SSD1306 nay do
 * chính Sdcard_Task/Oled_Task tự làm lúc khởi động, không còn ở đây (xem Sdcard_Task/oled.c) -
 * rồi mới bật ngắt nút bấm SAU CÙNG (xem lý do quan trọng ở bước Button_Init() bên dưới).
 * Bản thân app_main chạy trên 1 task hệ thống riêng của ESP-IDF (không phải 1 trong 4 task
 * kể trên).
 * @param
 * @return
 */
void app_main(void)
{
    /* srm init - phải gọi đầu tiên, trước khi tạo bất kỳ task nào (tạo mutex bảo vệ
       registry lúc còn đơn luồng, xem Srm_Init() trong srm.c) */
    Srm_Init();

    /* player manager init - chỉ set state ban đầu (gsPlayerContext), CHƯA tạo task, xem
       PlayerManager_Init() trong player_manager.c */
    PlayerManager_Init();

    /* mp3 init - phải gọi trước khi tạo Mp3_Task (tạo xMp3CommandQueue) và trước khi
       PlayerManager_Task có thể gửi notification tới xMp3TaskHandle */
    Mp3_Init();

    /* sdcard init - phải gọi trước khi tạo Sdcard_Task (tạo xSdCommandQueue, dùng bởi
       Oled_Task để gửi SDCARD_CMD_GET_SINGLE_FRAME qua SRM mỗi khi cần 1 frame animation) */
    Sdcard_Init();

    /* spi init - khởi tạo SPI_HOST_ID đúng 1 lần cho toàn hệ thống, PHẢI gọi trước khi tạo
       Mp3_Task (vs1053_init() bên trong chỉ add device lên bus có sẵn, không tự khởi tạo bus
       nữa - xem spi.h để biết lý do tách ra khỏi vs1053.c: sau này device SPI khác chỉ cần tự
       add lên cùng bus này, không phải sửa lại chỗ khởi tạo bus). Lỗi ở đây không chặn
       app_main - Mp3_Task vẫn được tạo bình thường, tự phát hiện lỗi qua vs1053_init() trả về
       khác ESP_OK và tự halt (xem Mp3_Task trong mp3.c), không cần thêm 1 đường xử lý lỗi mới
       ở đây */
    if (Spi_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Spi_Init failed - devices on SPI_HOST_ID (vd VS1053) will not work");
    }

    /* i2c init - khởi tạo I2C_PORT_NUM đúng 1 lần cho toàn hệ thống, PHẢI gọi trước khi tạo
       Oled_Task (Oled_Init() bên trong chỉ add device lên bus có sẵn qua i2c_device_add(),
       không tự khởi tạo bus nữa - cùng lý do tách Spi_Init() ở trên, xem i2c.h). LƯU Ý khác
       Spi_Init(): thư viện SSD1306 dùng ESP_ERROR_CHECK() nội bộ cho i2c_master_bus_add_device()
       (driver/ssd1306/ssd1306_i2c_new.c) - nếu I2c_Init() lỗi, Oled_Task add device lên bus
       NULL sẽ khiến hệ thống reboot ngay, không halt êm như Mp3_Task/vs1053_init(); log ở đây
       chỉ để thấy nguyên nhân trước khi reboot, không ngăn được crash */
    if (I2c_Init() != ESP_OK)
    {
        ESP_LOGE(TAG, "I2c_Init failed - Oled_Task will crash the system on add device");
    }

    /* Tạo đủ cả 4 task chính TRƯỚC KHI bật ngắt nút bấm (xem Button_Init() ở cuối hàm).
       Oled_Task tự khởi tạo màn hình SSD1306 (biến local, xem Oled_Task trong oled.c) và
       Sdcard_Task tự mount/quét thẻ SD (xem Sdcard_Task trong sdcard.c) ngay lúc khởi động
       của chính nó, không còn làm ở app_main - 2 task báo nhau qua Srm_OledNotifyBootStatus()
       (srm.c) nếu mount/quét lỗi, không task nào cần pvParameters riêng nên đều truyền NULL */
    xTaskCreatePinnedToCore(Sdcard_Task, "sdcard_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_SDCARD, &xSdTaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(Mp3_Task, "mp3_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_MP3, &xMp3TaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(PlayerManager_Task, "player_manager_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_PLAYER, &xPlayerManagerTaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(Oled_Task, "oled_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_OLED, &xOledTaskHandle, OLED_TASK_CORE);

    /* Button_Init() PHẢI gọi SAU CÙNG, sau khi cả 4 task đã tạo xong: các ISR nút bấm
       (Button_NextISR/Button_PrevISR/Button_PlayISR - xem button.c) gọi thẳng
       xTaskNotifyFromISR(xPlayerManagerTaskHandle, ...) ngay khi có ngắt, và
       PlayerManager_Task lúc xử lý sự kiện lại gọi tiếp xTaskNotify(xOledTaskHandle/
       xSdTaskHandle/xMp3TaskHandle, ...). Nếu bật ngắt TRƯỚC khi các task handle này tồn tại
       (vd Button_Init() gọi sớm như code cũ, trước khi xTaskCreatePinnedToCore() gán giá trị
       cho các handle) và người dùng bấm nút đúng lúc đó, các hàm xTaskNotify(FromISR) sẽ nhận
       tham số NULL -> FreeRTOS dereference NULL -> crash ngay lúc boot. Đặt ở đây đảm bảo mọi
       handle đều đã hợp lệ trước khi CÓ THỂ nhận được bất kỳ ngắt nút bấm nào. */
    Button_Init();

    /* Task main (pin core 0, priority ESP_TASK_MAIN_PRIO) đã hoàn thành nhiệm vụ khởi tạo,
       không còn việc gì để làm - để hàm return, ESP-IDF sẽ tự vTaskDelete(NULL) task main
       (xem main_task trong cpu_start.c), giải phóng luôn stack của nó thay vì giữ chết */
}
