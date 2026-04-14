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
    /* mount sd card */
    sdcard_mount();
    /* scan and get info of mp3 and frame */
    scan_sdcard_and_create_db("/sdcard");
    /* display on console */
    read_db_file();

    /* oled init */
    oled_init(&dev);
    /* display logo */
    /* ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 4, "Hello", 5, false); */

    /* button init */
    button_init();

    /* start run task */
   xTaskCreatePinnedToCore(sdcard_task, "sdcard_task", 4096, &dev, 5, NULL, 1);

    xTaskCreatePinnedToCore(button_task, "button_task", 4096, NULL, 5, NULL, 1);

    xTaskCreatePinnedToCore(oled_task, "oled_task", 4096, &dev, 5, NULL, 1);

    /* never to jumb */
    while(1);
}
