/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "ssd1306.h"
#include "oled.h"
#include "player_manager.h"
#include "menu.h"
#include "double_buffer.h"
#include "sync_frame.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Chu kỳ vẽ 1 frame animation khi đang PLAYING (ms) */
#define OLED_ANIMATION_DELAY_MS     40U

/* Số page của màn hình SSD1306 128x64 (mỗi page cao 8px) */
#define OLED_PAGE_COUNT             8U
#define OLED_PAGE_WIDTH             128U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
TaskHandle_t xOledTaskHandle = NULL;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Buffer chứa 1 frame animation đọc từ double buffer trước khi vẽ lên màn hình */
static uint8_t gau8Frame[FRAME_SIZE];

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_DrawFrame
 * Vẽ 1 frame (đã đọc từ double buffer) lên toàn bộ màn hình OLED theo từng page
 * @param dev: con trỏ device SSD1306
 * @param au8Frame: buffer chứa dữ liệu frame cần vẽ, kích thước FRAME_SIZE
 * @return
 */
static void Oled_DrawFrame(SSD1306_t *dev, uint8_t *au8Frame)
{
    for (uint32_t lu32Page = 0; lu32Page < OLED_PAGE_COUNT; lu32Page++)
    {
        ssd1306_display_image(dev, lu32Page, 0, &au8Frame[lu32Page * OLED_PAGE_WIDTH], OLED_PAGE_WIDTH);
    }
}

/**
 * @brief Oled_PlayAnimation
 * Vẽ animation liên tục trong lúc đang PLAYING, đồng bộ theo thời gian giải mã mp3.
 * Mỗi vòng lặp đều check non-blocking (timeout = 0) xem PlayerManager_Task có gửi
 * notification mới không, để không bị delay khi cần thoát ra xử lý sự kiện khác
 * (đổi bài / pause / về menu). Vòng lặp cũng tự thoát khi hết trạng thái PLAY.
 * @param dev: con trỏ device SSD1306
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu thoát do hết trạng thái PLAY
 */
static bool Oled_PlayAnimation(SSD1306_t *dev, uint32_t *pu32NotifyValue)
{
    /* Còn animation khi: đang ở màn hình PLAYING và button không phải PAUSE
       (PLAY/PLAY_NEW/NEXT/PREV đều coi là đang chạy animation) */
    while ((gsPlayerContext.mainState == MAIN_STATE_PLAYING) && (gsPlayerContext.buttonState != BTN_STATE_PAUSE))
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Lấy frame_index đồng bộ theo thời gian giải mã mp3 hiện tại rồi vẽ lên màn hình */
        uint32_t lu32FrameIndex = syncFrameWithMp3();
        double_buffer_get_frame(lu32FrameIndex, gau8Frame);
        Oled_DrawFrame(dev, gau8Frame);

        vTaskDelay(pdMS_TO_TICKS(OLED_ANIMATION_DELAY_MS));
    }

    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_Init
 * Khởi tạo màn hình OLED (I2C + SSD1306)
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Oled_Init(SSD1306_t *dev)
{
    i2c_master_init(dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    ssd1306_init(dev, CONFIG_WIDTH, CONFIG_HEIGHT);
}

/**
 * @brief Oled_Task
 * Task chính điều khiển màn hình OLED. Chờ Oled_EventType_e từ PlayerManager_Task
 * qua task notification thay vì tự poll trạng thái, để phản ứng ngay khi có sự kiện
 * và không tốn CPU khi không có gì thay đổi.
 * @param pvParameters: con trỏ SSD1306_t* của màn hình, truyền vào lúc tạo task
 * @return
 */
void Oled_Task(void *pvParameters)
{
    SSD1306_t *dev = (SSD1306_t *)pvParameters;
    uint32_t lu32NotifyValue;
    /* true khi lu32NotifyValue đã có sẵn 1 sự kiện cần xử lý (do Oled_PlayAnimation vừa trả về),
       lúc đó không cần chờ notification mới nữa mà xử lý ngay ở vòng lặp kế tiếp */
    bool lbHasPendingEvent = false;

    /* Vẽ menu lần đầu khi khởi động */
    Menu_Draw(dev);

    while (1)
    {
        /* Chỉ block chờ notification mới khi chưa có sẵn sự kiện nào đang chờ xử lý */
        if (!lbHasPendingEvent)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32NotifyValue, portMAX_DELAY);
        }
        lbHasPendingEvent = false;

        switch ((Oled_EventType_e)lu32NotifyValue)
        {
            case OLED_EVENT_MENU_DRAW:
                /* menu.c tự đọc gsPlayerContext.cursor, không cần đồng bộ thủ công ở đây nữa */
                Menu_UpdateScroll();
                Menu_Draw(dev);
                break;

            case OLED_EVENT_SONG_CHANGED:
                /* Bài mới -> reset lại mốc thời gian đồng bộ trước khi chạy animation */
                initSync();
                lbHasPendingEvent = Oled_PlayAnimation(dev, &lu32NotifyValue);
                break;

            case OLED_EVENT_PLAY_PAUSE:
                /* Không đổi bài -> chạy tiếp (hoặc dừng) animation dựa theo buttonState hiện tại */
                lbHasPendingEvent = Oled_PlayAnimation(dev, &lu32NotifyValue);
                break;

            default:
                break;
        }
    }
}
