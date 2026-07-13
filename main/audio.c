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

SSD1306_t dev;

void app_main(void)
{
    /* srm init - phải gọi đầu tiên, trước khi tạo bất kỳ task nào (tạo mutex bảo vệ
       registry lúc còn đơn luồng, xem Srm_Init() trong srm.c) */
    Srm_Init();

    /* oled init - đặt trước các bước thao tác thẻ SD để có thể hiển thị lỗi ngay lên màn
       hình nếu mount/scan thẻ SD thất bại, thay vì chỉ log ra UART console (trước đây hệ
       thống boot "thành công" trong im lặng dù không mount được thẻ/không có bài hát nào) */
    Oled_Init(&dev);

    /* mount sd card */
    esp_err_t leSdMountResult = Sdcard_Mount();
    /* scan and get info of mp3 and frame */
    Sdcard_ScanAndCreateDb("/sdcard");
    /* display on console */
    Sdcard_ReadDbFile();

    /* Thẻ SD lỗi hoặc quét xong nhưng không tìm thấy bài hát hợp lệ nào (thiếu file .bin
       đi kèm...) -> báo rõ cho người dùng biết trên OLED, giữ hiển thị vài giây trước khi
       Oled_Task khởi động và vẽ đè màn hình menu (rỗng) lên trên */
    if ((leSdMountResult != ESP_OK) || (gu16SongCount == 0))
    {
        ssd1306_clear_screen(&dev, false);
        if (leSdMountResult != ESP_OK)
        {
            ssd1306_display_text(&dev, 3, "SD Card Error!", 14, false);
        }
        else
        {
            ssd1306_display_text(&dev, 3, "No songs found", 14, false);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* button ini nên để init sau cùng tránh quá trình boot user giữ nút */
    /* button init */
    Button_Init();

    /* player manager init */
    PlayerManager_Init();

    /* mp3 init - phải gọi trước khi tạo Mp3_Task (tạo xMp3CommandQueue) và trước khi
       PlayerManager_Task có thể gửi notification tới xMp3TaskHandle */
    Mp3_Init();

    /* sdcard init - phải gọi trước khi tạo Sdcard_Task (tạo xSdCommandQueue, dùng bởi
       Oled_Task để gửi SDCARD_CMD_GET_FRAME qua SRM mỗi khi cần 1 frame animation) */
    Sdcard_Init();

    /* start run task */
   xTaskCreatePinnedToCore(Sdcard_Task, "sdcard_task", 4096, &dev, 5, &xSdTaskHandle, 1);

    xTaskCreatePinnedToCore(Mp3_Task, "mp3_task", 4096, NULL, 5, &xMp3TaskHandle, 1);

    xTaskCreatePinnedToCore(PlayerManager_Task, "player_manager_task", 4096, NULL, 5, &xPlayerManagerTaskHandle, 1);

    xTaskCreatePinnedToCore(Oled_Task, "oled_task", 4096, &dev, 5, &xOledTaskHandle, 1);

    /* Task main (pin core 0, priority ESP_TASK_MAIN_PRIO) đã hoàn thành nhiệm vụ khởi tạo,
       không còn việc gì để làm - phải nhường CPU (không dùng while(1); rỗng) để task Idle0
       được chạy và tự "feed" Task Watchdog Timer của chính nó (sdkconfig có
       CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y), nếu không TWDT sẽ báo lỗi liên tục mỗi
       CONFIG_ESP_TASK_WDT_TIMEOUT_S giây trong suốt vòng đời thiết bị */
    vTaskDelay(portMAX_DELAY);
}
