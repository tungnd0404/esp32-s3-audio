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

/* Tốc độ khung hình animation thật (dữ liệu frame.bin), lấy theo FRAME_PER_SECOND (config.h) */
#define OLED_ANIMATION_FRAME_INTERVAL_MS   (1000U / FRAME_PER_SECOND)

/* Chu kỳ vTaskDelay giữa các lần poll trong Oled_PlayAnimation: vẫn phải delay để nhường
   CPU cho task khác, nhưng poll nhanh gấp đôi tốc độ frame thật để bắt kịp thời điểm
   frame_index vừa đổi sớm hơn thay vì phải chờ tới gần hết 1 chu kỳ frame. Phần poll dư
   thừa (frame chưa đổi) được lọc bỏ, không vẽ lại, nhờ điều kiện so sánh với frame vẽ lần
   trước bên trong Oled_PlayAnimation */
#define OLED_ANIMATION_DELAY_MS            (OLED_ANIMATION_FRAME_INTERVAL_MS / 2U)

/* Số page của màn hình SSD1306 128x64 (mỗi page cao 8px) */
#define OLED_PAGE_COUNT             8U
#define OLED_PAGE_WIDTH             128U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
TaskHandle_t xOledTaskHandle = NULL;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* Trạng thái hiển thị riêng của OLED, suy ra từ PlayerManager_ButtonStateType_e nhận được
   qua notification. Chỉ dùng nội bộ oled.c để switch cho tường minh (đọc thẳng ra "đang vẽ
   menu" hay "đang chạy animation"...), không phải kiểu dữ liệu đi qua kênh notification
   (kênh truyền vẫn dùng thẳng PlayerManager_ButtonStateType_e, không thêm enum sự kiện riêng). */
typedef enum {
    OLED_DISPLAY_MENU,          /* Vẽ lại menu */
    OLED_DISPLAY_SONG_CHANGED,  /* Đổi bài -> reset đồng bộ rồi chạy animation cho bài mới */
    OLED_DISPLAY_PLAY_PAUSE     /* Tiếp tục/tạm dừng animation, không đổi bài */
} Oled_DisplayStateType_e;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Buffer chứa 1 frame animation đọc từ double buffer trước khi vẽ lên màn hình */
static uint8_t gau8Frame[FRAME_SIZE];

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_GetDisplayState
 * Suy ra Oled_DisplayStateType_e tương ứng từ buttonState nhận được qua notification,
 * gom các buttonState có cùng cách xử lý bên OLED vào chung 1 display state.
 * @param buttonState: giá trị PlayerManager_ButtonStateType_e nhận từ PlayerManager_Task
 * @return Oled_DisplayStateType_e tương ứng
 */
static Oled_DisplayStateType_e Oled_GetDisplayState(PlayerManager_ButtonStateType_e buttonState)
{
    switch (buttonState)
    {
        case BTN_STATE_NEXT:
        case BTN_STATE_PREV:
        case BTN_STATE_PLAY_NEW:
            return OLED_DISPLAY_SONG_CHANGED;

        case BTN_STATE_PLAY:
        case BTN_STATE_PAUSE:
            return OLED_DISPLAY_PLAY_PAUSE;

        case BTN_STATE_UP:
        case BTN_STATE_DOWN:
        case BTN_STATE_BACK_MENU:
        case BTN_STATE_IDLE:
        default:
            return OLED_DISPLAY_MENU;
    }
}

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
    /* Frame hiện tại */
    uint32_t lu32FrameIndex = 0;
    /* Frame vừa vẽ lần gần nhất, dùng để tránh vẽ lại khi frame_index chưa đổi.
       Khởi tạo UINT32_MAX (không phải chỉ số frame hợp lệ) để đảm bảo frame đầu tiên của
       lần gọi này luôn được vẽ, không bị hiểu nhầm là "trùng frame" với lần gọi trước đó */
    uint32_t lu32LastDrawnFrameIndex = UINT32_MAX;

    /* Còn animation khi: đang ở màn hình PLAYING và playbackState không phải PAUSE.
       Dùng playbackState (tồn tại liên tục) thay vì buttonState (chỉ tồn tại trong
       khoảnh khắc xử lý 1 lần bấm nút rồi tự về IDLE) */
    while ((gsPlayerContext.mainState == MAIN_STATE_PLAYING) && (gsPlayerContext.playbackState != PLAYBACK_STATE_PAUSE))
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Lấy frame_index đồng bộ theo thời gian giải mã mp3 hiện tại */
        lu32FrameIndex = SyncFrame_GetFrameIndex();

        /* Chỉ đọc double buffer + vẽ lại khi frame thực sự đổi so với lần vẽ trước,
           tránh tốn CPU và băng thông I2C/SPI để vẽ lại đúng 1 frame nhiều lần liên tiếp
           (do OLED_ANIMATION_DELAY_MS poll nhanh hơn tốc độ frame thật) */
        if (lu32FrameIndex != lu32LastDrawnFrameIndex)
        {
            double_buffer_get_frame(lu32FrameIndex, gau8Frame);
            Oled_DrawFrame(dev, gau8Frame);
            lu32LastDrawnFrameIndex = lu32FrameIndex;
        }

        /* Vẫn phải delay dù frame có đổi hay không, để nhường CPU cho task khác
           (sdcard_task, player_manager_task...) thay vì busy-poll liên tục */
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
 * Task chính điều khiển màn hình OLED.
 *
 * Cơ chế nhận sự kiện:
 * - Không tự poll trạng thái theo chu kỳ, mà "ngủ" (xTaskNotifyWait, portMAX_DELAY) chờ
 *   PlayerManager_Task gửi task notification, tiết kiệm CPU khi không có gì thay đổi và
 *   phản ứng ngay lập tức khi có sự kiện.
 * - Giá trị notification nhận được chính là gsPlayerContext.buttonState (kiểu
 *   PlayerManager_ButtonStateType_e) tại thời điểm PlayerManager_Task gửi đi -> không cần
 *   thêm 1 enum sự kiện riêng cho kênh notification giữa 2 task.
 * - Giá trị đó được map qua Oled_GetDisplayState() sang Oled_DisplayStateType_e (state riêng
 *   của OLED, chỉ dùng nội bộ file này) để switch bên dưới đọc tường minh theo đúng ý nghĩa
 *   hiển thị, thay vì phải nhớ nhóm BTN_STATE_* nào ứng với hành vi nào.
 *
 * 3 nhánh xử lý theo Oled_DisplayStateType_e:
 * - OLED_DISPLAY_MENU: cursor di chuyển hoặc double click thoát PLAYING về MENU
 *   -> Menu_UpdateScroll() + Menu_Draw() vẽ lại danh sách bài hát.
 * - OLED_DISPLAY_SONG_CHANGED: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> SyncFrame_Init() reset lại mốc đồng bộ thời gian giải mã, rồi chạy Oled_PlayAnimation().
 * - OLED_DISPLAY_PLAY_PAUSE: bấm Play/Pause hoặc auto-return từ MENU về lại PLAYING, bài
 *   không đổi -> chạy thẳng Oled_PlayAnimation(), không reset đồng bộ.
 *
 * Cơ chế lbHasPendingNotify: Oled_PlayAnimation() có thể thoát sớm vì vừa nhận 1 notification
 * mới (thay vì thoát vì hết trạng thái PLAY) và đã đọc giá trị đó vào lu32NotifyValue. Nếu vòng
 * lặp ngoài gọi xTaskNotifyWait() lần nữa ngay lúc đó thì giá trị vừa nhận sẽ bị mất (thay bằng
 * giá trị chờ tiếp theo). lbHasPendingNotify = true báo cho vòng lặp biết: bỏ qua bước chờ,
 * xử lý ngay lu32NotifyValue đang có sẵn ở vòng lặp kế tiếp.
 *
 * @param pvParameters: con trỏ SSD1306_t* của màn hình, truyền vào lúc tạo task
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Oled_Task(void *pvParameters)
{
    /* pvParameters được audio.c truyền vào lúc tạo task, thực chất là &dev (SSD1306_t)
       khai báo global bên main -> ép kiểu lại để dùng xuyên suốt task */
    SSD1306_t *dev = (SSD1306_t *)pvParameters;
    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32NotifyValue;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32NotifyValue đã có sẵn
       1 giá trị mới cần xử lý ngay (do Oled_PlayAnimation vừa trả về) */
    bool lbHasPendingNotify = false;

    /* Vẽ menu lần đầu khi khởi động, trước khi vào vòng lặp chính */
    Menu_Draw(dev);

    /* Vòng lặp chính của task, chạy vô hạn cho tới khi thiết bị tắt nguồn */
    while (1)
    {
        /* Check xem có notify nào đang pending không? nếu không thì chờ notify mới,
           chờ vô thời hạn (portMAX_DELAY) vì task không có việc gì khác để làm */
        if (lbHasPendingNotify == false)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32NotifyValue, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Oled_PlayAnimation() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Map buttonState vừa nhận sang display state riêng của OLED rồi rẽ nhánh xử lý */
        switch (Oled_GetDisplayState((PlayerManager_ButtonStateType_e)lu32NotifyValue))
        {
            case OLED_DISPLAY_MENU:
                /* menu.c tự đọc gsPlayerContext.cursor, không cần đồng bộ thủ công ở đây nữa */
                /* Tính lại cửa sổ cuộn (gi32StartIndex) theo cursor hiện tại */
                Menu_UpdateScroll();
                /* Vẽ lại toàn bộ danh sách bài hát lên màn hình */
                Menu_Draw(dev);
                break;

            case OLED_DISPLAY_SONG_CHANGED:
                /* Bài mới -> reset mốc thời gian đồng bộ giải mã mp3 về 0 trước khi vẽ animation,
                   tránh dùng nhầm mốc thời gian của bài phát trước đó */
                SyncFrame_Init();
                /* Chạy vòng vẽ animation cho bài mới; nếu bị ngắt giữa chừng do có notify khác
                   tới thì lbHasPendingNotify sẽ được set true để xử lý tiếp ở vòng lặp kế */
                lbHasPendingNotify = Oled_PlayAnimation(dev, &lu32NotifyValue);
                break;

            case OLED_DISPLAY_PLAY_PAUSE:
                /* Không đổi bài nên không cần SyncFrame_Init() lại -> chạy thẳng animation,
                   Oled_PlayAnimation() tự đọc gsPlayerContext.playbackState để biết
                   nên tiếp tục vẽ (PLAY) hay dừng ngay vòng lặp đầu tiên (PAUSE) */
                lbHasPendingNotify = Oled_PlayAnimation(dev, &lu32NotifyValue);
                break;

            default:
                /* Không có case nào khớp (về lý thuyết không xảy ra) -> bỏ qua */
                break;
        }
    }
}
