#include "ssd1306.h"
#include "oled.h"
#include "button.h"
#include "menu.h"

void oled_init(SSD1306_t * dev)
{
    i2c_master_init(dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    ssd1306_init(dev, CONFIG_WIDTH, CONFIG_HEIGHT);
}

//================== HANDLE BUTTON ==================
static void oled_handle(SSD1306_t *dev) 
{
    if (stateMain == STATE_MENU)
    {
        switch (stateButton) {
        case STATE_UP:
            cursor--;
            if (cursor < 0) {
                cursor = song_count - 1;
            }
            update_scroll();
            draw_menu(dev);
            stateButton = STATE_IDLE;
            break;

        case STATE_DOWN:
            cursor++;
            if (cursor >= song_count) {
                cursor = 0;
            }
            update_scroll();
            draw_menu(dev);
            stateButton = STATE_IDLE;
            break;

        case STATE_PLAY_NEW:
            stateButton = STATE_IDLE;
            break;

        case STATE_IDLE:
        default:
            // không làm gì
            break;
        }
    }
    // state playing
    else
    {
        switch (statePlay) {
        case STATE_PLAY_PLAY:
            
            break;

        case STATE_PLAY_PAUSE:
            
            break;

        default:
            // không làm gì
            break;
        }
    }
}

// ================== PUBLIC API ==================

void oled_task(void *pvParameters) 
{
    SSD1306_t *dev = (SSD1306_t *)pvParameters;

    draw_menu(dev);

    while (1) {
        oled_handle(dev);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}