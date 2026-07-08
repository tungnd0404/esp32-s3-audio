#include "ssd1306.h"
#include "oled.h"
#include "player_manager.h"
#include "menu.h"
#include "double_buffer.h"
#include "sync_frame.h"


/* task handler oled_task */
TaskHandle_t oled_taskHandle = NULL;

static uint8_t frame[FRAME_SIZE];

void oled_init(SSD1306_t * dev)
{
    i2c_master_init(dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    ssd1306_init(dev, CONFIG_WIDTH, CONFIG_HEIGHT);
}

void oled_draw_frame(SSD1306_t * dev, uint8_t *frame)
{
    for (int page = 0; page < 8; page++) {
        ssd1306_display_image(dev, page, 0, &frame[page * 128], 128);
    }
}

//================== HANDLE BUTTON ==================
static void oled_handle(SSD1306_t *dev) 
{
    if (stateMain == STATE_MENU)
    {
        switch (stateButton) 
        {
        case STATE_UP:
            update_scroll();
            draw_menu(dev);
            /* Update trạng thái button */
            stateButton = STATE_IDLE;
            break;

        case STATE_DOWN:
            update_scroll();
            draw_menu(dev);
            /* Update trạng thái button */
            stateButton = STATE_IDLE;
            break;

        case STATE_PLAY_NEW:
            stateButton = STATE_IDLE;
            break;

        case STATE_IDLE:
        default:
            /* không làm gì */
            break;
        }
    }
    /* State playing */
    else
    {
        switch (stateButton) {
        case STATE_PLAY:
            int frame_index = syncFrameWithMp3();
            double_buffer_get_frame(frame_index,frame);
            oled_draw_frame(dev, frame);
            vTaskDelay(pdMS_TO_TICKS(40));
            break;

        case STATE_PAUSE:
            /* có thể vẽ nút pause ở đây */
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