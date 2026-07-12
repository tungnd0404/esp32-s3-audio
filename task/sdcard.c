/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sdcard.h"
#include "config.h"
#include "player_manager.h"
#include "double_buffer.h"
#include "srm.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Đường dẫn file database chứa danh sách bài hát, tạo bởi Sdcard_ScanAndCreateDb() */
#define SDCARD_DB_PATH              "/sdcard/songs.db"

/* Thời gian tối đa chờ lệnh mới trên xSdCommandQueue (ms) mỗi vòng lặp trong Sdcard_Task.
   Chờ có giới hạn (không dùng portMAX_DELAY) để mỗi vòng lặp còn tranh thủ check
   non-blocking xem có bài mới cần chuyển sang không (xem Sdcard_Task) */
#define SDCARD_LOAD_WAIT_MS         50U

/* Số request tối đa có thể chờ xử lý trong xSdCommandQueue cùng lúc */
#define SDCARD_COMMAND_QUEUE_LENGTH 4U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Sdcard_Task */
TaskHandle_t xSdTaskHandle = NULL;

/* Danh sách tên bài hát trong RAM, dùng cho menu hiển thị */
Sdcard_SongInfoType_s gaSongList[SDCARD_MAX_SONGS];

/* Tổng số bài hát tìm thấy trên thẻ nhớ */
uint16_t gu16SongCount = 0;

/* Hàng đợi lệnh dùng chung tới Sdcard_Task, tạo trong Sdcard_Init() vì Sdcard_Task là owner
   dữ liệu double buffer animation (xem driver/buffer/double_buffer.c) */
QueueHandle_t xSdCommandQueue = NULL;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* Trạng thái nạp dữ liệu riêng của Sdcard_Task, suy ra từ PlayerManager_ButtonStateType_e
   nhận được qua notification. Cùng kiểu thiết kế với Oled_DisplayStateType_e (oled.c):
   chỉ dùng nội bộ file này để switch cho tường minh, không phải kiểu dữ liệu đi qua kênh
   notification (kênh truyền vẫn dùng thẳng PlayerManager_ButtonStateType_e). */
typedef enum {
    SDCARD_LOAD_SONG_CHANGED,  /* Đổi bài -> mở lại double buffer, nạp frame cho bài mới */
    SDCARD_LOAD_NONE           /* Sự kiện không liên quan (di chuyển cursor, play/pause...) -> bỏ qua */
} Sdcard_LoadStateType_e;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "SDCARD";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Sdcard_GetLoadState
 * Suy ra Sdcard_LoadStateType_e tương ứng từ buttonState nhận được qua notification.
 * Sdcard_Task chỉ thực sự quan tâm sự kiện ĐỔI BÀI (Next/Prev lúc đang phát, hoặc chọn
 * bài mới từ MENU) - di chuyển cursor trong MENU hay play/pause không làm thay đổi bài
 * đang cần nạp dữ liệu nên được gộp chung vào SDCARD_LOAD_NONE.
 * @param buttonState: giá trị PlayerManager_ButtonStateType_e nhận từ PlayerManager_Task
 * @return Sdcard_LoadStateType_e tương ứng
 */
static Sdcard_LoadStateType_e Sdcard_GetLoadState(PlayerManager_ButtonStateType_e buttonState)
{
    switch (buttonState)
    {
        case BTN_STATE_NEXT:
        case BTN_STATE_PREV:
        case BTN_STATE_PLAY_NEW:
            return SDCARD_LOAD_SONG_CHANGED;

        case BTN_STATE_UP:
        case BTN_STATE_DOWN:
        case BTN_STATE_BACK_MENU:
        case BTN_STATE_PLAY:
        case BTN_STATE_PAUSE:
        case BTN_STATE_IDLE:
        default:
            return SDCARD_LOAD_NONE;
    }
}

/**
 * @brief Sdcard_HandleCommand
 * Xử lý 1 request nhận được từ xSdCommandQueue (xem srm.h - kiến trúc Owner Task): dựa vào
 * cmdId gọi đúng hàm DoubleBuffer_* tương ứng (Sdcard_Task là owner duy nhất được phép ghi
 * vào double buffer animation, xem driver/buffer/double_buffer.c).
 * @param pRequest: request nhận được từ xSdCommandQueue
 * @return
 */
static void Sdcard_HandleCommand(const Srm_Message_s *pRequest)
{
    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        case SDCARD_CMD_PRELOAD_BUFFER:
            /* Trả lời NGAY (ack "đã nhận lệnh", không phải "đã nạp xong") rồi mới thực sự
               nạp - để bên gửi (DoubleBuffer_GetFrame(), đang giữ gxMutexDoubleBuffer lúc
               gọi Srm_SendCommand cho lệnh này) không phải chờ tới lúc nạp xong thật (có
               thể mất vài-vài chục ms đọc thẻ SD) mới được trả lời. Nhờ trả lời trước khi
               gọi DoubleBuffer_Preload(), hàm đó có lấy gxMutexDoubleBuffer trễ hơn cũng
               không sao - bên gửi lúc này đã nhận được ack và không còn giữ mutex chờ nữa,
               không có rủi ro deadlock */
            Srm_Reply(pRequest, 1U);
            DoubleBuffer_Preload();
            break;

        case SDCARD_CMD_LOAD_MISSING_FRAME:
        {
            /* Lệnh gửi qua Srm_SendCommand() - bên gửi đang chờ, luôn phải trả lời để họ
               không phải đợi hết timeout mới biết kết quả */
            bool lbSuccess = DoubleBuffer_LoadFrame(pRequest->payload);
            Srm_Reply(pRequest, (uint32_t)lbSuccess);
            break;
        }

        default:
            /* cmdId lạ (chưa định nghĩa xử lý) -> vẫn trả lời 0 để bên gửi (nếu có chờ)
               không phải đợi hết timeout mới coi là lỗi */
            Srm_Reply(pRequest, 0U);
            break;
    }
}

/**
 * @brief Sdcard_LoadCurrentSong
 * Mở double buffer cho bài hát ĐANG THỰC SỰ PHÁT (gsPlayerContext.currentSong) rồi liên
 * tục xử lý các lệnh nạp dữ liệu frame animation gửi tới qua xSdCommandQueue
 * (SDCARD_CMD_PRELOAD_BUFFER/SDCARD_CMD_LOAD_MISSING_FRAME, xem Sdcard_HandleCommand), cho
 * tới khi có bài mới cần chuyển sang. Cùng khuôn mẫu với Oled_PlayAnimation() (oled.c):
 * mỗi vòng lặp đều check non-blocking notification để không trễ khi cần đổi bài, kết quả
 * trả về qua tham số ra để vòng lặp ngoài không bị mất giá trị notification vừa nhận.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi
 *        hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu thoát do lỗi
 *         đọc thông tin bài hát (Sdcard_GetSongByIndex thất bại, vd DB lỗi)
 */
static bool Sdcard_LoadCurrentSong(uint32_t *pu32NotifyValue)
{
    Sdcard_SongDbType_s lSong;

    /* Không dùng gsPlayerContext.cursor - vì cursor có thể đã di chuyển sang bài khác nếu
       người dùng đang duyệt MENU trong lúc nhạc vẫn phát nền (xem tính năng auto-return) */
    if (Sdcard_GetSongByIndex((uint16_t)gsPlayerContext.currentSong, &lSong) == false)
    {
        return false;
    }

    /* Đóng buffer của bài cũ (nếu có), mở lại cho bài mới */
    DoubleBuffer_Close();
    DoubleBuffer_Open(lSong.framePath);

    while (1)
    {
        Srm_Message_s lRequest;

        /* Chờ có giới hạn thời gian (không portMAX_DELAY) để còn quay lại check
           non-blocking phía dưới xem có tín hiệu đổi bài mới không. Có lệnh tới thì xử lý
           ngay, không cần chờ hết SDCARD_LOAD_WAIT_MS */
        if (xQueueReceive(xSdCommandQueue, &lRequest, pdMS_TO_TICKS(SDCARD_LOAD_WAIT_MS)) == pdTRUE)
        {
            Sdcard_HandleCommand(&lRequest);
        }

        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }
    }
}

/**
 * @brief Sdcard_HasExtension
 * Kiểm tra tên file có đúng phần mở rộng cần tìm không
 * @param name: tên file bao gồm extension (vd "song.mp3")
 * @param ext: phần mở rộng cần so khớp, bao gồm dấu chấm (vd ".mp3")
 * @return true nếu đúng, false nếu sai hoặc tên file không có phần mở rộng
 */
static bool Sdcard_HasExtension(const char *name, const char *ext)
{
    const char *lDot = strrchr(name, '.');
    if (lDot == NULL)
    {
        return false;
    }

    return (strcasecmp(lDot, ext) == 0);
}

/**
 * @brief Sdcard_RemoveExtension
 * Copy tên file sang dest, bỏ phần mở rộng (phần từ dấu chấm cuối cùng trở đi)
 * @param pDest: buffer đích để lưu kết quả, kích thước destSize byte
 * @param destSize: kích thước pDest, tránh tràn bộ nhớ khi tên file dài bất thường
 * @param pSrc: tên file nguồn bao gồm extension (vd "song.mp3")
 * @return
 */
static void Sdcard_RemoveExtension(char *pDest, size_t destSize, const char *pSrc)
{
    char *lDot;

    /* Copy có giới hạn kích thước, luôn đảm bảo kết thúc bằng '\0' */
    snprintf(pDest, destSize, "%s", pSrc);

    lDot = strrchr(pDest, '.');
    if (lDot != NULL)
    {
        *lDot = '\0';
    }
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Sdcard_Init
 * Khởi tạo module Sdcard: tạo xSdCommandQueue để các module khác (double_buffer.c) gửi
 * lệnh vào. Gọi trước khi tạo Sdcard_Task.
 * @param
 * @return
 */
void Sdcard_Init(void)
{
    xSdCommandQueue = xQueueCreate(SDCARD_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
}

/**
 * @brief Sdcard_Mount
 * Mount thẻ nhớ SD ở chế độ 1-bit (SDMMC)
 * @param
 * @return ESP_OK nếu mount thành công
 */
esp_err_t Sdcard_Mount(void)
{
    esp_err_t lRet;
    sdmmc_card_t *pCard;

    /* macro tạo config mặc định */
    sdmmc_host_t lHost = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t lSlotConfig = SDMMC_SLOT_CONFIG_DEFAULT();

    /* Dùng SD 1-bit -> không dùng các chân data D1/D2/D3 */
    lSlotConfig.width = 1;
    lSlotConfig.d1 = -1;
    lSlotConfig.d2 = -1;
    lSlotConfig.d3 = -1;

    /* Cấu hình chân GPIO thật của board */
    lSlotConfig.clk = SD_CLK;
    lSlotConfig.cmd = SD_CMD;
    lSlotConfig.d0 = SD_D0;

    /* format_if_mount_failed = false -> báo lỗi thay vì tự format thẻ khi mount thất bại
       max_files: số file tối đa mở đồng thời
       allocation_unit_size: kích thước cluster FAT filesystem (16KB) */
    esp_vfs_fat_sdmmc_mount_config_t lMountConfig =
    {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    lRet = esp_vfs_fat_sdmmc_mount("/sdcard", &lHost, &lSlotConfig, &lMountConfig, &pCard);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed");
        return lRet;
    }

    ESP_LOGI(TAG, "SD card mounted");

    /* In thông tin thẻ nhớ ra console */
    sdmmc_card_print_info(stdout, pCard);

    return ESP_OK;
}

/**
 * @brief Sdcard_ScanAndCreateDb
 * Quét toàn bộ thư mục gốc thẻ nhớ, tìm các file .mp3 có file .bin (frame animation)
 * đi kèm, ghi thành file database "/sdcard/songs.db" và nạp tên bài hát vào gaSongList
 * @param basePath: thư mục gốc cần quét (vd "/sdcard")
 * @return
 */
void Sdcard_ScanAndCreateDb(const char *basePath)
{
    DIR *pDir;
    FILE *pDb;
    struct dirent *pEntry;

    pDir = opendir(basePath);
    if (pDir == NULL)
    {
        printf("Cannot open dir\n");
        return;
    }

    pDb = fopen(SDCARD_DB_PATH, "wb");
    if (pDb == NULL)
    {
        printf("Cannot open db file\n");
        closedir(pDir);
        return;
    }

    /* Duyệt từng entry trong thư mục cho tới khi hết file hoặc đầy gaSongList */
    while (((pEntry = readdir(pDir)) != NULL) && (gu16SongCount < SDCARD_MAX_SONGS))
    {
        char lName[64];
        char lMp3Path[128];
        char lBinPath[128];
        FILE *pBinFile;
        Sdcard_SongDbType_s lRecord;

        printf("Found: %s\n", pEntry->d_name);

        /* Chỉ lấy file .mp3, bỏ qua file khác */
        if (!Sdcard_HasExtension(pEntry->d_name, ".mp3"))
        {
            continue;
        }

        Sdcard_RemoveExtension(lName, sizeof(lName), pEntry->d_name);

        snprintf(lMp3Path, sizeof(lMp3Path), "%s/%s.mp3", basePath, lName);
        snprintf(lBinPath, sizeof(lBinPath), "%s/%s.bin", basePath, lName);

        /* Bài hát chỉ hợp lệ nếu có đủ file .bin (frame animation) đi kèm */
        pBinFile = fopen(lBinPath, "rb");
        if (pBinFile == NULL)
        {
            printf("Missing bin for %s\n", lName);
            continue;
        }
        fclose(pBinFile);

        /* Ghi 1 bản ghi vào database */
        memset(&lRecord, 0, sizeof(lRecord));
        snprintf(lRecord.songPath, sizeof(lRecord.songPath), "%s", lMp3Path);
        snprintf(lRecord.framePath, sizeof(lRecord.framePath), "%s", lBinPath);
        fwrite(&lRecord, sizeof(Sdcard_SongDbType_s), 1, pDb);

        /* Lưu song song vào RAM để menu hiển thị, không cần đọc lại file mỗi lần vẽ */
        snprintf(gaSongList[gu16SongCount].songName, sizeof(gaSongList[gu16SongCount].songName), "%s", lName);

        #if defined DEVELOPER_CONFIGURATION
            printf("Added: %s\n", lName);
        #endif

        gu16SongCount++;
    }

    fclose(pDb);
    closedir(pDir);

    printf("Total songs: %d\n", gu16SongCount);
}

/**
 * @brief Sdcard_ReadDbFile
 * In toàn bộ nội dung file database "/sdcard/songs.db" ra console (debug)
 * @param
 * @return
 */
void Sdcard_ReadDbFile(void)
{
    FILE *pDb;
    Sdcard_SongDbType_s lRecord;

    /* Hàm này luôn được biên dịch (dù chỉ hữu ích lúc debug) vì sdcard.h khai báo và
       audio.c gọi không điều kiện - nếu bọc cả hàm trong #if DEVELOPER_CONFIGURATION như
       bản gốc, build release (không định nghĩa DEVELOPER_CONFIGURATION) sẽ lỗi linker vì
       gọi tới hàm không tồn tại. Chỉ nội dung in ra mới cần thiết cho debug, nên chỉ bọc
       phần đó nếu muốn, còn bản thân hàm luôn tồn tại. */
    pDb = fopen(SDCARD_DB_PATH, "rb");
    if (pDb == NULL)
    {
        printf("Cannot open db\n");
        return;
    }

    while (fread(&lRecord, sizeof(Sdcard_SongDbType_s), 1, pDb) == 1)
    {
        printf("MP3: %s\n", lRecord.songPath);
        printf("BIN: %s\n", lRecord.framePath);
    }

    fclose(pDb);
}

/**
 * @brief Sdcard_GetSongByIndex
 * Đọc 1 bản ghi trong file database "/sdcard/songs.db" theo chỉ số
 * @param index: chỉ số bài hát cần lấy (0..gu16SongCount-1)
 * @param pOut: struct nhận thông tin bài hát
 * @return true nếu lấy thành công, false nếu lỗi (index ngoài phạm vi, không mở được file...)
 */
bool Sdcard_GetSongByIndex(uint16_t index, Sdcard_SongDbType_s *pOut)
{
    FILE *pDb;
    long lOffset;

    if ((index >= gu16SongCount) || (pOut == NULL))
    {
        return false;
    }

    pDb = fopen(SDCARD_DB_PATH, "rb");
    if (pDb == NULL)
    {
        printf("Open DB failed\n");
        return false;
    }

    /* Nhảy thẳng tới bản ghi thứ index, không cần đọc tuần tự từ đầu */
    lOffset = (long)index * (long)sizeof(Sdcard_SongDbType_s);
    if (fseek(pDb, lOffset, SEEK_SET) != 0)
    {
        printf("fseek failed\n");
        fclose(pDb);
        return false;
    }

    if (fread(pOut, sizeof(Sdcard_SongDbType_s), 1, pDb) != 1)
    {
        printf("fread failed\n");
        fclose(pDb);
        return false;
    }

    fclose(pDb);
    return true;
}

/**
 * @brief Sdcard_Task
 * Task nạp dữ liệu frame animation từ thẻ nhớ vào double buffer cho bài đang phát.
 * Cùng khuôn thuật toán với Oled_Task (oled.c) để các task trong hệ thống đọc thống nhất,
 * dễ theo dõi:
 *
 * Cơ chế nhận sự kiện:
 * - Không tự poll trạng thái theo chu kỳ, mà "ngủ" (xTaskNotifyWait, portMAX_DELAY) chờ
 *   PlayerManager_Task gửi task notification, tiết kiệm CPU khi không có gì thay đổi và
 *   phản ứng ngay lập tức khi có sự kiện.
 * - Giá trị notification nhận được chính là gsPlayerContext.buttonState (kiểu
 *   PlayerManager_ButtonStateType_e) tại thời điểm PlayerManager_Task gửi đi -> không cần
 *   thêm 1 enum sự kiện riêng cho kênh notification giữa 2 task.
 * - Giá trị đó được map qua Sdcard_GetLoadState() sang Sdcard_LoadStateType_e (state riêng
 *   của Sdcard_Task, chỉ dùng nội bộ file này) để switch bên dưới đọc tường minh, thay vì
 *   phải nhớ buttonState nào cần xử lý.
 *
 * 2 nhánh xử lý theo Sdcard_LoadStateType_e:
 * - SDCARD_LOAD_SONG_CHANGED: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> chạy Sdcard_LoadCurrentSong() mở lại double buffer và nạp dữ liệu cho bài mới.
 * - SDCARD_LOAD_NONE: sự kiện không liên quan tới việc đổi bài (di chuyển cursor MENU,
 *   play/pause, peek MENU...) -> bỏ qua, không làm gì.
 *
 * Cơ chế lbHasPendingNotify: Sdcard_LoadCurrentSong() có thể thoát sớm vì vừa nhận 1
 * notification mới (thay vì thoát vì lỗi đọc DB) và đã đọc giá trị đó vào lu32NotifyValue.
 * Nếu vòng lặp ngoài gọi xTaskNotifyWait() lần nữa ngay lúc đó thì giá trị vừa nhận sẽ bị
 * mất (thay bằng giá trị chờ tiếp theo). lbHasPendingNotify = true báo cho vòng lặp biết:
 * bỏ qua bước chờ, xử lý ngay lu32NotifyValue đang có sẵn ở vòng lặp kế tiếp.
 *
 * @param pvParameters
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Sdcard_Task(void *pvParameters)
{
    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32NotifyValue;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32NotifyValue đã có sẵn
       1 giá trị mới cần xử lý ngay (do Sdcard_LoadCurrentSong vừa trả về) */
    bool lbHasPendingNotify = false;

    /* Khởi tạo double buffer trước khi dùng tới DoubleBuffer_Open()/DoubleBuffer_Close() -
       truyền xSdCommandQueue để double_buffer.c biết gửi SDCARD_CMD_* tới đâu (xem
       DoubleBuffer_Init() trong double_buffer.h) */
    DoubleBuffer_Init(xSdCommandQueue);

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
           bên dưới nếu Sdcard_LoadCurrentSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Map buttonState vừa nhận sang load state riêng của Sdcard_Task rồi rẽ nhánh xử lý */
        switch (Sdcard_GetLoadState((PlayerManager_ButtonStateType_e)lu32NotifyValue))
        {
            case SDCARD_LOAD_SONG_CHANGED:
                /* Chạy vòng nạp dữ liệu cho bài mới; nếu bị ngắt giữa chừng do có notify
                   khác tới thì lbHasPendingNotify sẽ được set true để xử lý tiếp ở vòng
                   lặp kế, không mất giá trị notification vừa nhận */
                lbHasPendingNotify = Sdcard_LoadCurrentSong(&lu32NotifyValue);
                break;

            case SDCARD_LOAD_NONE:
            default:
                /* Sự kiện không liên quan tới đổi bài -> bỏ qua */
                break;
        }
    }
}
