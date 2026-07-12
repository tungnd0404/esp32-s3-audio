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

SSD1306_t dev;

void app_main(void)
{
    /* srm init - phải gọi đầu tiên, trước khi tạo bất kỳ task nào (tạo mutex bảo vệ
       registry lúc còn đơn luồng, xem Srm_Init() trong srm.c) */
    Srm_Init();

    /* mount sd card */
    sdcard_mount();
    /* scan and get info of mp3 and frame */
    scan_sdcard_and_create_db("/sdcard");
    /* display on console */
    read_db_file();

    /* oled init */
    Oled_Init(&dev);
    /* display logo */
    /* ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 4, "Hello", 5, false); */

    /* button ini nên để init sau cùng tránh quá trình boot user giữ nút */
    /* button init */
    Button_Init();

    /* player manager init */
    PlayerManager_Init();

    /* start run task */
   xTaskCreatePinnedToCore(sdcard_task, "sdcard_task", 4096, &dev, 5, &sd_taskHandle, 1);

    xTaskCreatePinnedToCore(PlayerManager_Task, "player_manager_task", 4096, NULL, 5, &xPlayerManagerTaskHandle, 1);

    xTaskCreatePinnedToCore(Oled_Task, "oled_task", 4096, &dev, 5, &xOledTaskHandle, 1);

    /* never to jumb */
    while(1);
}
