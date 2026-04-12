#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "sdcard.h"
#include "oled.h"
#include "menu.h"
#include "button.h"

SSD1306_t dev;

void app_main(void)
{
    sdcard_mount();
    scan_sdcard_and_create_db("/sdcard");
    read_db_file();

    oled_init(&dev);
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 4, "Hello", 5, false);

    button_init();

    xTaskCreate(
    sdcard_task,     // hàm task
    "sdcard_task",        // tên task
    4096,               // stack size (bytes)
    &dev,               // truyền tham số vào (pvParameters)
    5,                  // priority
    NULL                // handle (có thể NULL)
);
    
    xTaskCreatePinnedToCore(
    button_task,
    "button_task",
    4096,
    NULL,
    5,
    NULL,
    0   // core 0 hoặc 1
);
    xTaskCreate(
    oled_task,     // hàm task
    "oled_task",        // tên task
    4096,               // stack size (bytes)
    &dev,               // truyền tham số vào (pvParameters)
    5,                  // priority
    NULL                // handle (có thể NULL)
);

    printf("Reset reason: %d\n", esp_reset_reason());   

}
