/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "ssd1306.h"
#include "oled.h"
#include "player_manager.h"
#include "menu.h"
#include "double_buffer.h"
#include "sync_frame.h"
#include "sdcard.h"
#include "srm.h"

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

/* Thời gian tối đa chờ Sdcard_Task phản hồi SDCARD_CMD_GET_SINGLE_FRAME (ms) - Sdcard_Task tự nạp
   trước/nạp gấp bên trong lúc xử lý lệnh này nếu cần (xem DoubleBuffer_GetFrame), có thể
   mất vài-vài chục ms đọc thẻ SD nên timeout để rộng, không phải giá trị chờ bình thường */
#define OLED_GET_FRAME_TIMEOUT_MS          5000U

/* Số request tối đa có thể chờ xử lý trong xOledCommandQueue cùng lúc */
#define OLED_COMMAND_QUEUE_LENGTH   4U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Oled_Task */
TaskHandle_t xOledTaskHandle = NULL;

/* Hàng đợi lệnh dùng chung tới Oled_Task, tạo trong Oled_Init() vì Oled_Task là owner - xem
   giải thích đầy đủ ở khai báo extern trong oled.h */
QueueHandle_t xOledCommandQueue = NULL;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Buffer chứa 1 frame animation đọc từ double buffer trước khi vẽ lên màn hình. Để static
   (thay vì biến local trong Oled_PlayAnimation) là CỐ Ý: chỉ Oled_Task đụng vào buffer này
   (Srm_SdcardGetSingleFrame() blocking - không có 2 request chồng nhau trong cùng 1 task nên
   không lo ghi đè lẫn lộn), và nếu để local thì 1024 byte sẽ trừ thẳng vào stack của
   Oled_Task (chỉ 4096 byte, xem xTaskCreatePinnedToCore trong audio.c) thay vì nằm ở .bss -
   rủi ro tràn stack, không đáng đánh đổi chỉ để "cục bộ hoá" biến. */
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
 * @brief Oled_HandleCommand
 * Xử lý 1 request nhận được từ xOledCommandQueue (xem srm.h - kiến trúc Owner Task), luôn
 * trả lời qua Srm_Reply() dù cmdId có hợp lệ hay không (bên gửi đang chờ, không trả lời thì
 * họ sẽ phải đợi hết timeout mới coi là lỗi). Hiện CHƯA có case nào - chưa có lệnh
 * (Srm_CommandType_e) nào owner bởi Oled_Task, chỉ dựng sẵn khung xử lý (xem giải thích ở
 * khai báo extern xOledCommandQueue trong oled.h).
 * @param dev: con trỏ device SSD1306 (đã Oled_Task khởi tạo)
 * @param pRequest: request nhận được từ xOledCommandQueue
 * @return
 */
static void Oled_HandleCommand(SSD1306_t *dev, const Srm_Message_s *pRequest)
{
    (void)dev;

    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        default:
            /* cmdId lạ (chưa định nghĩa xử lý cho Oled_Task) -> vẫn trả lời E_NOT_OK để bên
               gửi không phải chờ hết timeout, tránh trường hợp SRM_CMD_INVALID lọt qua
               được lúc gửi */
            Srm_Reply(pRequest, (uint32_t)E_NOT_OK);
            break;
    }
}

/**
 * @brief Oled_ServicePendingCommand
 * Check non-blocking (timeout = 0) xem xOledCommandQueue có request nào đang chờ không, có
 * thì xử lý ngay. Gọi mỗi vòng lặp trong Oled_PlayAnimation() - cùng khuôn mẫu với
 * Mp3_ServicePendingCommand() (mp3.c). LƯU Ý: hàm này KHÔNG được gọi khi Oled_Task đang rảnh
 * (đang chờ notify ở vòng lặp ngoài Oled_Task, xTaskNotifyWait dùng portMAX_DELAY) - lúc đó
 * request tới xOledCommandQueue sẽ không có ai xử lý cho tới vòng vẽ animation kế tiếp.
 * @param dev: con trỏ device SSD1306 (đã Oled_Task khởi tạo)
 * @return
 */
static void Oled_ServicePendingCommand(SSD1306_t *dev)
{
    Srm_Message_s lRequest;

    if (xQueueReceive(xOledCommandQueue, &lRequest, 0) == pdTRUE)
    {
        Oled_HandleCommand(dev, &lRequest);
    }
}

/**
 * @brief Oled_PlayAnimation
 * Vẽ animation liên tục trong lúc đang PLAYING, đồng bộ theo thời gian giải mã mp3.
 * Chỉ nên được gọi khi playbackState đang là PLAYBACK_STATE_PLAY - PAUSE có hàm riêng
 * Oled_DrawPauseIcon(), không đi qua đây nữa. Có check phòng thủ ngay đầu hàm: nếu lúc gọi
 * playbackState không phải PLAY (gọi sai chỗ) thì thoát ngay, không vẽ frame nào. Mỗi vòng
 * lặp bên trong đều check non-blocking (timeout = 0) xem PlayerManager_Task có gửi
 * notification mới không, để không bị delay khi cần thoát ra xử lý sự kiện khác (đổi bài /
 * pause / về menu) - đây là cách duy nhất vòng lặp thoát, vì mọi lần đổi mainState/
 * playbackState bên PlayerManager_Task đều luôn kèm 1 xTaskNotify tới Oled_Task ngay sau đó.
 * @param dev: con trỏ device SSD1306
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu gọi sai lúc
 *         playbackState không phải PLAY (không vẽ frame nào)
 */
static bool Oled_PlayAnimation(SSD1306_t *dev, uint32_t *pu32NotifyValue)
{
    /* Phòng thủ: hàm này chỉ dành cho lúc đang PLAY, không phải nơi xử lý PAUSE */
    if (gsPlayerContext.playbackState != PLAYBACK_STATE_PLAY)
    {
        return false;
    }

    /* Frame hiện tại */
    uint32_t lu32FrameIndex = 0;
    /* Frame vừa vẽ lần gần nhất, dùng để tránh vẽ lại khi frame_index chưa đổi.
       Khởi tạo UINT32_MAX (không phải chỉ số frame hợp lệ) để đảm bảo frame đầu tiên của
       lần gọi này luôn được vẽ, không bị hiểu nhầm là "trùng frame" với lần gọi trước đó */
    uint32_t lu32LastDrawnFrameIndex = UINT32_MAX;

    /* Vòng lặp vẽ animation, chạy tới khi có notification mới cần xử lý */
    while (1)
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Tranh thủ trả lời các request đang chờ trong lúc đang chạy vòng lặp này, xem
           Oled_ServicePendingCommand() */
        Oled_ServicePendingCommand(dev);

        /* Lấy frame_index đồng bộ theo thời gian giải mã mp3 hiện tại */
        lu32FrameIndex = SyncFrame_GetFrameIndex();

        /* Chỉ xin frame mới + vẽ lại khi frame thực sự đổi so với lần vẽ trước, tránh tốn
           CPU/băng thông I2C/SPI lẫn 1 round-trip SRM không cần thiết để vẽ lại đúng 1
           frame nhiều lần liên tiếp (do OLED_ANIMATION_DELAY_MS poll nhanh hơn tốc độ
           frame thật) */
        if (lu32FrameIndex != lu32LastDrawnFrameIndex)
        {
            /* Xin Sdcard_Task (owner duy nhất của double buffer, xem double_buffer.h) 1
               frame mới qua SRM - Sdcard_Task tự nạp trước/nạp gấp nếu cần rồi ghi thẳng
               vào gau8Frame (truyền thẳng làm tham số, không cần đăng ký trước) trước khi
               trả lời, nên chỉ vẽ khi chắc chắn nhận được dữ liệu mới - tránh vẽ dữ liệu
               cũ/rác nếu timeout hay lỗi đọc thẻ SD */
            if (Srm_SdcardGetSingleFrame(lu32FrameIndex, gau8Frame,
                                    pdMS_TO_TICKS(OLED_GET_FRAME_TIMEOUT_MS)) == E_OK)
            {
                Oled_DrawFrame(dev, gau8Frame);
                lu32LastDrawnFrameIndex = lu32FrameIndex;
            }
        }

        /* Vẫn phải delay dù frame có đổi hay không, để nhường CPU cho task khác
           (Sdcard_Task, PlayerManager_Task...) thay vì busy-poll liên tục */
        vTaskDelay(pdMS_TO_TICKS(OLED_ANIMATION_DELAY_MS));
    }
}

/**
 * @brief Oled_DrawPauseIcon
 * TODO: vẽ thêm ký hiệu PAUSE đè lên frame hiện tại của màn hình OLED khi playbackState
 * chuyển sang PLAYBACK_STATE_PAUSE (case BTN_STATE_PAUSE trong Oled_Task). Hiện chưa triển
 * khai - cần chặn task tương tự Oled_PlayAnimation (xTaskNotifyWait non-blocking mỗi vòng)
 * để thoát ngay khi có notification mới (Play lại / đổi bài / về menu), tránh block Oled_Task.
 * @param dev: con trỏ device SSD1306
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp
 */
static bool Oled_DrawPauseIcon(SSD1306_t *dev, uint32_t *pu32NotifyValue)
{
    (void)dev;
    (void)pu32NotifyValue;
    /* TODO: implement */
    return false;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Oled_Init
 * Khởi tạo màn hình OLED (I2C + SSD1306), tạo xOledCommandQueue để các module khác (nếu sau
 * này có) gửi lệnh vào. Gọi trước khi tạo Oled_Task.
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Oled_Init(SSD1306_t *dev)
{
    i2c_master_init(dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
    ssd1306_init(dev, CONFIG_WIDTH, CONFIG_HEIGHT);
    xOledCommandQueue = xQueueCreate(OLED_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
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
 * - Switch bên dưới xử lý trực tiếp trên buttonState, không qua enum trung gian.
 *
 * 4 nhánh xử lý theo buttonState:
 * - BTN_STATE_NEXT/PREV/PLAY_NEW: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> SyncFrame_Init() reset lại mốc đồng bộ thời gian giải mã, rồi chạy Oled_PlayAnimation().
 * - BTN_STATE_PLAY: bấm Play hoặc auto-return từ MENU về lại PLAYING lúc đang PLAY, bài
 *   không đổi -> chạy thẳng Oled_PlayAnimation(), không reset đồng bộ.
 * - BTN_STATE_PAUSE: bấm Pause hoặc auto-return từ MENU về lại PLAYING lúc đang PAUSE
 *   -> Oled_DrawPauseIcon() (TODO) vẽ thêm ký hiệu PAUSE lên frame hiện tại, không chạy animation.
 * - Các buttonState còn lại (cursor di chuyển hoặc double click thoát PLAYING về MENU)
 *   -> Menu_UpdateScroll() + Menu_Draw() vẽ lại danh sách bài hát.
 *
 * Cơ chế lbHasPendingNotify: Oled_PlayAnimation()/Oled_DrawPauseIcon() có thể thoát sớm vì vừa
 * nhận 1 notification mới và đã đọc giá trị đó vào lu32button_evt. Nếu vòng lặp ngoài gọi
 * xTaskNotifyWait() lần nữa ngay lúc đó thì giá trị vừa nhận sẽ bị mất (thay bằng giá trị chờ
 * tiếp theo). lbHasPendingNotify = true báo cho vòng lặp biết: bỏ qua bước chờ, xử lý ngay
 * lu32button_evt đang có sẵn ở vòng lặp kế tiếp.
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
    uint32_t lu32button_evt;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32button_evt đã có sẵn
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
            xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Oled_PlayAnimation()/Oled_DrawPauseIcon() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Rẽ nhánh trực tiếp trên buttonState vừa nhận, không qua state trung gian */
        switch ((PlayerManager_ButtonStateType_e)lu32button_evt)
        {
            case BTN_STATE_NEXT:
            case BTN_STATE_PREV:
            case BTN_STATE_PLAY_NEW:
                /* Bài mới -> reset mốc thời gian đồng bộ giải mã mp3 về 0 trước khi vẽ animation,
                   tránh dùng nhầm mốc thời gian của bài phát trước đó */
                SyncFrame_Init();
                /* Chạy vòng vẽ animation cho bài mới; nếu bị ngắt giữa chừng do có notify khác
                   tới thì lbHasPendingNotify sẽ được set true để xử lý tiếp ở vòng lặp kế */
                lbHasPendingNotify = Oled_PlayAnimation(dev, &lu32button_evt);
                break;

            case BTN_STATE_PLAY:
                /* Không đổi bài nên không cần SyncFrame_Init() lại -> chạy thẳng animation */
                lbHasPendingNotify = Oled_PlayAnimation(dev, &lu32button_evt);
                break;

            case BTN_STATE_PAUSE:
                /* Không đổi bài, chỉ tạm dừng -> vẽ thêm ký hiệu PAUSE lên frame hiện tại
                   (TODO: xem Oled_DrawPauseIcon) thay vì tiếp tục animation */
                lbHasPendingNotify = Oled_DrawPauseIcon(dev, &lu32button_evt);
                break;

            case BTN_STATE_UP:
            case BTN_STATE_DOWN:
            case BTN_STATE_BACK_MENU:
            case BTN_STATE_IDLE:
            default:
                /* menu.c tự đọc gsPlayerContext.cursor, không cần đồng bộ thủ công ở đây nữa */
                /* Tính lại cửa sổ cuộn (gi32StartIndex) theo cursor hiện tại */
                Menu_UpdateScroll();
                /* Vẽ lại toàn bộ danh sách bài hát lên màn hình */
                Menu_Draw(dev);
                break;
        }
    }
}
