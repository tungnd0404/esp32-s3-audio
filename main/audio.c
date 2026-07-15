/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "sdcard.h"
#include "oled.h"
#include "menu.h"
#include "button.h"
#include "player_manager.h"
#include "srm.h"
#include "mp3.h"

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
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief app_main
 * Điểm vào chương trình (entry point ESP-IDF): khởi tạo tuần tự từng module (thứ tự PHẢI
 * đúng như dưới đây - xem comment tại từng bước), mount + quét thẻ SD lấy danh sách bài hát,
 * tạo đủ 4 task chính (Sdcard_Task/Mp3_Task/PlayerManager_Task/Oled_Task), rồi mới bật ngắt
 * nút bấm SAU CÙNG (xem lý do quan trọng ở bước Button_Init() bên dưới). Bản thân app_main
 * chạy trên 1 task hệ thống riêng của ESP-IDF (không phải 1 trong 4 task kể trên).
 * @param
 * @return
 */
void app_main(void)
{
    /* Device SSD1306 dùng chung cho toàn hệ thống - CỐ Ý để local (không phải global) vì chỉ
       audio.c cần đụng tới (Oled_Task nhận địa chỉ qua pvParameters, không qua extern global
       theo tên như gsPlayerContext). An toàn vì app_main() KHÔNG BAO GIỜ return (kết thúc
       bằng vTaskDelay(portMAX_DELAY) vô hạn ở cuối hàm) - stack frame của app_main, và do đó
       địa chỉ &dev, còn sống suốt vòng đời thiết bị. NẾU SAU NÀY sửa app_main để return sớm ở
       bất kỳ nhánh nào (vd early-return khi lỗi nghiêm trọng), &dev mà Oled_Task đang giữ sẽ
       thành dangling pointer - phải đưa dev trở lại thành global (hoặc static) trước khi làm
       vậy. */
    SSD1306_t dev;

    /* srm init - phải gọi đầu tiên, trước khi tạo bất kỳ task nào (tạo mutex bảo vệ
       registry lúc còn đơn luồng, xem Srm_Init() trong srm.c) */
    Srm_Init();

    /* oled init - đặt trước các bước thao tác thẻ SD để có thể hiển thị lỗi ngay lên màn
       hình nếu mount/scan thẻ SD thất bại, thay vì chỉ log ra UART console (trước đây hệ
       thống boot "thành công" trong im lặng dù không mount được thẻ/không có bài hát nào) */
    Oled_Init(&dev);

    /* Mount thẻ SD, quét toàn bộ bài hát hợp lệ (có đủ cặp .mp3 + .bin) vào database, rồi in
       ra console để debug. Chỉ quét/đọc DB khi mount thành công - mount fail thì "/sdcard"
       không truy cập được, gọi tiếp cũng chỉ tự thất bại vô ích (Sdcard_ScanAndCreateDb tự
       log "Cannot open dir" rồi return, không crash, nhưng không cần thiết) */
    esp_err_t lRet = Sdcard_Mount();
    if (lRet == ESP_OK)
    {
        Sdcard_ScanAndCreateDb("/sdcard");
        Sdcard_ReadDbFile();
    }

    /* Thẻ SD lỗi hoặc quét xong nhưng không tìm thấy bài hát hợp lệ nào (thiếu file .bin
       đi kèm...) -> báo rõ cho người dùng biết trên OLED, giữ hiển thị vài giây trước khi
       Oled_Task khởi động và vẽ đè màn hình menu (rỗng) lên trên */
    if ((lRet != ESP_OK) || (gu16SongCount == 0))
    {
        ssd1306_clear_screen(&dev, false);
        if (lRet != ESP_OK)
        {
            ssd1306_display_text(&dev, 3, "SD Card Error!", 14, false);
        }
        else
        {
            ssd1306_display_text(&dev, 3, "No songs found", 14, false);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* player manager init - chỉ set state ban đầu (gsPlayerContext), CHƯA tạo task, xem
       PlayerManager_Init() trong player_manager.c */
    PlayerManager_Init();

    /* mp3 init - phải gọi trước khi tạo Mp3_Task (tạo xMp3CommandQueue) và trước khi
       PlayerManager_Task có thể gửi notification tới xMp3TaskHandle */
    Mp3_Init();

    /* sdcard init - phải gọi trước khi tạo Sdcard_Task (tạo xSdCommandQueue, dùng bởi
       Oled_Task để gửi SDCARD_CMD_GET_SINGLE_FRAME qua SRM mỗi khi cần 1 frame animation) */
    Sdcard_Init();

    /* Tạo đủ cả 4 task chính TRƯỚC KHI bật ngắt nút bấm (xem Button_Init() ở cuối hàm) -
       pvParameters chỉ truyền &dev cho Oled_Task (owner thật sự của màn hình); Sdcard_Task/
       Mp3_Task/PlayerManager_Task không đụng gì tới SSD1306 nên truyền NULL, không mượn &dev
       cho có vì gây hiểu lầm hàm đó cũng dùng màn hình */
    xTaskCreatePinnedToCore(Sdcard_Task, "sdcard_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_SDCARD, &xSdTaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(Mp3_Task, "mp3_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_MP3, &xMp3TaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(PlayerManager_Task, "player_manager_task", AUDIO_TASK_STACK_SIZE, NULL,
                             AUDIO_TASK_PRIORITY_PLAYER, &xPlayerManagerTaskHandle, AUDIO_TASK_CORE);

    xTaskCreatePinnedToCore(Oled_Task, "oled_task", AUDIO_TASK_STACK_SIZE, &dev,
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
       không còn việc gì để làm - phải nhường CPU (không dùng while(1); rỗng) để task Idle0
       được chạy và tự "feed" Task Watchdog Timer của chính nó (sdkconfig có
       CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y), nếu không TWDT sẽ báo lỗi liên tục mỗi
       CONFIG_ESP_TASK_WDT_TIMEOUT_S giây trong suốt vòng đời thiết bị */
    vTaskDelay(portMAX_DELAY);
}
