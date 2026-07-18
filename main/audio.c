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
#include "player_manager.h"
#include "srm.h"
#include "pcm_player.h"
#include "i2c.h"
#include "sdmmc.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Stack size (word... thực ra byte, xTaskCreatePinnedToCore nhận usStackDepth theo byte trên
   ESP-IDF) dùng chung cho cả 4 task - đủ dư cho nhu cầu hiện tại của từng task (I2S/I2C
   transaction structs cỡ vài trăm byte, không có buffer lớn nào nằm trên stack - các buffer
   1024/4096/65536 byte của double buffer/ring buffer đều là static/global, xem double_buffer.c/
   ring_buffer.c). Nếu sau này thêm việc gì tốn stack hơn, dùng uxTaskGetStackHighWaterMark()
   để đo thực tế thay vì đoán */
#define AUDIO_TASK_STACK_SIZE   4096U

/* Core dành RIÊNG cho Pcm_Task - task nhạy cảm độ trễ nhất trong hệ thống (chậm 1 nhịp ghi I2S
   là nghe giật/rè ngay). Không task nào khác được tạo trên core này, kể cả Sdcard_Task/
   PlayerManager_Task (dù priority thấp hơn đã đủ để Pcm_Task luôn được ưu tiên khi ready trên
   cùng core, nhưng dồn hẳn 1 core riêng loại bỏ hoàn toàn rủi ro Sdcard_Task giữ CPU hơi lâu
   trong 1 lời gọi FATFS/SDMMC dài mà không có điểm preempt, hoặc jitter do tick-scheduling
   khi chung core) */
#define PCM_TASK_CORE            0

/* Core chạy Sdcard_Task/PlayerManager_Task/Oled_Task - dồn chung vì đều không nhạy cảm độ
   trễ bằng Pcm_Task (Sdcard_Task đã được tách khỏi timing audio nhờ xPcmRingBuffer làm lớp
   đệm, PlayerManager_Task chỉ xử lý sự kiện nút bấm, Oled_Task thuần cosmetic). Priority vẫn
   giữ Sdcard_Task/PlayerManager_Task (5) cao hơn Oled_Task (4) trên core này để animation OLED
   không làm đói CPU của Sdcard_Task, tránh gián tiếp làm cạn xPcmRingBuffer */
#define OTHER_TASK_CORE          1

/* Priority tương đối giữa 4 task - CAO hơn = ưu tiên CPU hơn khi có tranh chấp (tất cả vẫn
   thấp hơn nhiều so với priority các task hệ thống ESP-IDF, vd WiFi task ~18-23, nên không
   ảnh hưởng tới phần lõi hệ thống dù project hiện chưa dùng WiFi):
   - Pcm_Task CAO NHẤT: nhạy cảm với độ trễ nhất trong 4 task - chậm 1 nhịp là nghe thấy giật/
     rè ngay lập tức, khác với animation/UI (chậm 1 frame gần như không ai nhận ra)
   - Sdcard_Task/PlayerManager_Task ngang Pcm_Task - Sdcard_Task cần chạy đều để không làm cạn
     xPcmRingBuffer (đặc biệt quan trọng hơn cả trước đây vì PCM cần thông lượng đọc thẻ SD
     gấp ~10 lần MP3 cũ), PlayerManager_Task cần phản hồi nút bấm nhanh, nhưng công việc mỗi
     lần đều ngắn nên không cần cao bằng Pcm_Task
   - Oled_Task THẤP NHẤT: chấp nhận được nếu bị trễ 1 chút do phải nhường CPU cho các task còn
     lại - animation lệch vài chục ms không ai để ý bằng audio giật */
#define AUDIO_TASK_PRIORITY_PCM      6U
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
 * đúng như dưới đây - xem comment tại từng bước), tạo đủ 4 task chính (Sdcard_Task/Pcm_Task/
 * PlayerManager_Task/Oled_Task, ĐÚNG THỨ TỰ - xem comment tại lời gọi xTaskCreatePinnedToCore
 * cho Oled_Task) - mount/quét thẻ SD và khởi tạo màn hình SSD1306 nay do chính Sdcard_Task/
 * Oled_Task tự làm lúc khởi động, không còn ở đây (xem Sdcard_Task/oled.c). Kênh I2S (MAX98357A)
 * cũng do chính Pcm_Task tự khởi tạo lúc bắt đầu chạy (xem Max98357a_Init() trong
 * driver/max98357a/max98357a.c, gọi từ Pcm_Task) - KHÔNG cần Spi_Init()/Gpio_Init() riêng ở
 * đây nữa như thời VS1053 (I2S không phải bus dùng chung cho nhiều thiết bị như I2C, chỉ
 * Pcm_Task dùng nên tự sở hữu trọn vòng đời của chính nó). Ngắt nút bấm (Button_Init()) không
 * còn bật ở app_main() nữa - Oled_Task (task cuối cùng được tạo) tự gọi ngay sau khi vẽ xong
 * menu lần đầu, xem Oled_Task trong oled.c. Bản thân app_main chạy trên 1 task hệ thống riêng
 * của ESP-IDF (không phải 1 trong 4 task kể trên).
 * @param
 * @return
 */
void app_main(void)
{
   /* srm init - phải gọi đầu tiên, trước khi tạo bất kỳ task nào (tạo mutex bảo vệ
      registry lúc còn đơn luồng, xem Srm_Init() trong srm.c) */
   Srm_Init();

   /* PlayerManager_Init()/Sdcard_Init()/Pcm_Init() không còn gọi ở đây nữa - mỗi task nay tự
      gọi hàm init của chính mình lúc khởi động (đầu PlayerManager_Task/Sdcard_Task/Pcm_Task),
      xem player_manager.c/sdcard.c/pcm_player.c để biết lý do từng hàm an toàn khi gọi theo
      kiểu này (không cần đồng bộ hoá gì thêm với các task khác) */

   /* i2c init - khởi tạo I2C0_PORT_NUM đúng 1 lần cho toàn hệ thống, PHẢI gọi trước khi tạo
      Oled_Task (Oled_Init() bên trong chỉ add device lên bus có sẵn qua ssd1306_add_i2c_device(),
      không tự khởi tạo bus nữa - xem i2c.h). LƯU Ý: thư viện SSD1306 dùng ESP_ERROR_CHECK()
      nội bộ cho i2c_master_bus_add_device() (driver/ssd1306/ssd1306_i2c.c) - nếu I2c_Init()
      lỗi, Oled_Task add device lên bus NULL sẽ khiến hệ thống reboot ngay, không halt êm như
      Pcm_Task/Max98357a_Init(); log ở đây chỉ để thấy nguyên nhân trước khi reboot, không ngăn
      được crash */
   if (I2c_Init() != ESP_OK)
   {
       ESP_LOGE(TAG, "I2c_Init failed - Oled_Task will crash the system on add device");
   }

   /* sdmmc init - chuẩn bị sẵn sdmmc_host_t/sdmmc_slot_config_t (chân/độ rộng bus theo
      board, xem sdmmc_config.h) vào biến global nội bộ của sdmmc.c, PHẢI gọi trước khi tạo
      Sdcard_Task (Sdcard_Mount() bên trong gọi Sdmmc_Mount(), dùng lại config đã chuẩn bị ở
      đây thay vì tự dựng lại - xem driver/sdmmc/sdmmc.c). Luôn trả ESP_OK (chỉ gán giá trị
      struct, chưa đụng phần cứng thật) nên không cần kiểm tra lỗi ở đây */
   Sdmmc_Init();

   /* Tạo đủ cả 4 task chính - THỨ TỰ NÀY QUAN TRỌNG, Oled_Task PHẢI LUÔN LÀ TASK CUỐI CÙNG
      được tạo: Oled_Task tự gọi Button_Init() ngay sau khi vẽ xong menu lần đầu (xem
      Oled_Task trong oled.c) thay vì gọi ở app_main() như trước - an toàn dựa trên việc cả 3
      handle còn lại (xPcmTaskHandle/xSdTaskHandle/xPlayerManagerTaskHandle) đã được gán
      XONG trước khi lệnh tạo Oled_Task này chạy tới (xTaskCreatePinnedToCore() gán handle
      đồng bộ trước khi return). Nếu sau này đổi thứ tự tạo task khiến Oled_Task không còn
      là task cuối, phải dời Button_Init() trở lại app_main() (sau toàn bộ 4
      xTaskCreatePinnedToCore()) - xem giải thích đầy đủ tại Button_Init() trong oled.c.
      Oled_Task tự khởi tạo màn hình SSD1306 (biến global, xem Oled_Task trong oled.c) và
      Sdcard_Task tự mount/quét thẻ SD (xem Sdcard_Task trong sdcard.c) ngay lúc khởi động
      của chính nó, không còn làm ở app_main - 2 task báo nhau qua Srm_OledNotifyBootStatus()
      (srm.c) nếu mount/quét lỗi, không task nào cần pvParameters riêng nên đều truyền NULL */
   xTaskCreatePinnedToCore(Pcm_Task, "pcm_task", AUDIO_TASK_STACK_SIZE, NULL,
                            AUDIO_TASK_PRIORITY_PCM, &xPcmTaskHandle, PCM_TASK_CORE);                          /* Priority 6 Core 0 */

   xTaskCreatePinnedToCore(Sdcard_Task, "sdcard_task", AUDIO_TASK_STACK_SIZE, NULL,
                            AUDIO_TASK_PRIORITY_SDCARD, &xSdTaskHandle, OTHER_TASK_CORE);                    /* Priority 5 Core 1 */

   xTaskCreatePinnedToCore(PlayerManager_Task, "player_manager_task", AUDIO_TASK_STACK_SIZE, NULL,
                            AUDIO_TASK_PRIORITY_PLAYER, &xPlayerManagerTaskHandle, OTHER_TASK_CORE);         /* Priority 5 Core 1 */

   /* Oled_Task PHẢI tạo SAU CÙNG - xem giải thích ở comment phía trên */
   xTaskCreatePinnedToCore(Oled_Task, "oled_task", AUDIO_TASK_STACK_SIZE, NULL,
                            AUDIO_TASK_PRIORITY_OLED, &xOledTaskHandle, OTHER_TASK_CORE);                    /* Priority 4 Core 1 */

   /* Task main (pin core 0, priority ESP_TASK_MAIN_PRIO) đã hoàn thành nhiệm vụ khởi tạo,
      không còn việc gì để làm - để hàm return, ESP-IDF sẽ tự vTaskDelete(NULL) task main
      (xem main_task trong cpu_start.c), giải phóng luôn stack của nó thay vì giữ chết */
}
